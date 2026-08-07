import AppKit
import AVFoundation
import CoreMedia

/// Plays a looping video into an `AVSampleBufferDisplayLayer`.
///
/// Memory discipline, in order of how much each one saves:
///
/// 1. **One frame in flight.** A display link pulls exactly one sample buffer
///    per tick and enqueues it with `DisplayImmediately`. There is no read-ahead
///    queue and no control timebase, so the decoder's pool stays at its floor
///    instead of the ~30 frames `AVPlayer` would happily buffer.
/// 2. **The decoder's own format, requested from nowhere.** 4:2:0 bi-planar is
///    1.5 bytes/px at 8-bit against BGRA's 4, and hardware HEVC decode produces
///    it without being asked. Naming the format instead made AVFoundation
///    revalidate every frame against it through a kernel round trip; see
///    `startReader`. YUV→RGB happens in the compositor for free either way.
/// 3. **Teardown on deactivate.** Losing visibility destroys the `AVAssetReader`
///    and its decompression session outright. The last frame stays composited by
///    WindowServer at no cost to us, and playback resumes from the saved time.
///
/// Assets are expected to have been normalised by `Transcoder` first: HEVC,
/// no audio track, capped resolution and frame rate.
final class VideoSource: NSObject, WallpaperSource {

    private let url: URL

    /// Tick rate for the display link. Seeded from the library's record but
    /// replaced by the track's real rate once the asset loads: pulling one frame
    /// per tick means a mismatch between the two plays the clip at the wrong
    /// speed, and the file is the authority, not the index.
    private var targetFPS: Int

    /// Recorded by the importer rather than sniffed from the file: we encoded
    /// it, so we know, and guessing from the format description is fragile.
    private let bitDepth: Int

    private let displayLayer = AVSampleBufferDisplayLayer()
    private var screen: NSScreen?

    /// Held strongly on purpose: `AVAssetTrack.asset` is a *weak* back
    /// reference, so without this the asset is deallocated as soon as
    /// `prepare()` returns. The reader would then be built over a different
    /// asset instance than the track belongs to, and `canAdd(_:)` would refuse
    /// the output — silently, with no error to log.
    private var asset: AVURLAsset?
    private var assetTrack: AVAssetTrack?
    private var assetDuration: CMTime = .zero
    private var loadFailed = false

    private var reader: AVAssetReader?
    private var trackOutput: AVAssetReaderTrackOutput?
    private var displayLink: CADisplayLink?

    /// Playback position preserved across teardown so resuming continues the
    /// loop instead of snapping back to frame zero.
    private var resumeTime: CMTime = .zero

    private var framesEnqueued = 0
    private var ticks = 0

    /// Guards against stacking decode work if a tick outruns the decoder.
    private var decodeInFlight = false
    private let decodeQueue = DispatchQueue(label: "livewall.decode", qos: .userInitiated)

    private(set) var isActive = false

    var layer: CALayer { displayLayer }

    var summary: String {
        url.deletingPathExtension().lastPathComponent
    }

    init(url: URL, fps: Int, bitDepth: Int = 8, fitMode: FitMode = .default) {
        self.url = url
        self.targetFPS = max(1, fps)
        self.bitDepth = bitDepth
        super.init()

        displayLayer.videoGravity = fitMode.videoGravity
        // Transparent rather than black, for two reasons: before the first frame
        // arrives this layer is all there is over the system wallpaper, and in
        // `.fit` the letterbox bars show that wallpaper instead of black. The
        // blending this costs is one full-screen composite the GPU does anyway.
        displayLayer.backgroundColor = NSColor.clear.cgColor
        displayLayer.isOpaque = false
    }

    /// Takes effect on the next composite; no decoder or reader involvement, so
    /// switching modes never interrupts playback.
    func setFitMode(_ mode: FitMode) {
        guard displayLayer.videoGravity != mode.videoGravity else { return }
        // Without this the gravity change animates the frame into place over the
        // default 0.25s, which on a desktop-level window reads as a glitch.
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        displayLayer.videoGravity = mode.videoGravity
        CATransaction.commit()
    }

    // MARK: - Asset loading

    /// Loads the video track once. Cached so that every subsequent
    /// activate/deactivate cycle rebuilds the reader synchronously and fast.
    func prepare(completion: @escaping (Bool) -> Void) {
        let asset = AVURLAsset(url: url, options: [
            AVURLAssetPreferPreciseDurationAndTimingKey: true
        ])
        Task {
            do {
                let tracks = try await asset.loadTracks(withMediaType: .video)
                guard let track = tracks.first else {
                    await MainActor.run { self.loadFailed = true; completion(false) }
                    return
                }
                let duration = try await asset.load(.duration)
                let rate = try await track.load(.nominalFrameRate)
                await MainActor.run {
                    self.asset = asset
                    self.assetTrack = track
                    self.assetDuration = duration
                    if rate > 0 {
                        let actual = Int(rate.rounded())
                        if actual != self.targetFPS {
                            Log.info("track is \(actual) fps, index said \(self.targetFPS) — using the track")
                        }
                        self.targetFPS = max(1, actual)
                    }
                    completion(true)
                }
            } catch {
                Log.error("failed to load \(self.url.lastPathComponent): \(error)")
                await MainActor.run { self.loadFailed = true; completion(false) }
            }
        }
    }

    // MARK: - WallpaperSource

    func updateGeometry(size: CGSize, scale: CGFloat) {
        displayLayer.frame = CGRect(origin: .zero, size: size)
        displayLayer.contentsScale = scale
    }

    func activate() {
        guard !isActive, !loadFailed else { return }
        isActive = true
        startReader(at: resumeTime)
        startDisplayLink()
        Log.info("video activated at \(CMTimeGetSeconds(resumeTime))s — \(Footprint.formatted())")
    }

    func deactivate() {
        guard isActive else { return }
        isActive = false

        stopDisplayLink()

        // Release the decode session and its pixel buffer pool. This is the
        // difference between "paused" and "costs nothing".
        decodeQueue.sync {
            reader?.cancelReading()
            reader = nil
            trackOutput = nil
        }

        // Deliberately no flush() — the last enqueued frame stays on screen so
        // reactivation is seamless.
        Log.info("video deactivated — \(Footprint.formatted())")
    }

    // MARK: - Reader

    private func startReader(at time: CMTime) {
        guard let track = assetTrack, let asset else {
            Log.error("startReader called before the asset finished loading")
            return
        }

        decodeQueue.sync {
            reader?.cancelReading()
            reader = nil
            trackOutput = nil

            do {
                let newReader = try AVAssetReader(asset: asset)

                // Resuming mid-loop: read from the saved position onward.
                if time > .zero && time < assetDuration {
                    newReader.timeRange = CMTimeRange(start: time, duration: .positiveInfinity)
                }

                // No output settings: take the decoder's own format.
                //
                // Naming a pixel format here looks harmless and costs a great
                // deal. Profiling showed 65 of 73 samples on this queue inside
                // `copyNextSampleBuffer`, and 47 of those in
                // CMVideoFormatDescriptionMatchesImageBuffer ->
                // copyIOSurfaceAttachment -> IOConnectCallMethod -> mach_msg2 —
                // a synchronous kernel round trip per frame to check the buffer
                // against the format we asked for, plus live
                // `videomediaconverter` threads doing the conversion. Asking for
                // nothing drops that to 4 samples and measured 3.96% -> 3.34%
                // CPU on the same clip.
                //
                // What we would have requested, we get anyway: hardware HEVC
                // decode hands back IOSurface-backed 4:2:0 bi-planar at the
                // file's own bit depth — 8-bit NV12 at 1.5 bytes/px, 10-bit at
                // 3 — and `Transcoder` guarantees every file here is HEVC. The
                // one thing this must never become is BGRA at 4 bytes/px, and
                // the decoder will not produce that unasked.
                let output = AVAssetReaderTrackOutput(track: track, outputSettings: nil)
                // We hand the buffer straight to the renderer and drop our
                // reference; no need for the reader to memcpy it first.
                output.alwaysCopiesSampleData = false

                guard newReader.canAdd(output) else {
                    Log.error("reader rejected the track output — track/asset mismatch?")
                    return
                }
                newReader.add(output)
                guard newReader.startReading() else {
                    Log.error("reader failed to start: \(String(describing: newReader.error))")
                    return
                }

                reader = newReader
                trackOutput = output
            } catch {
                Log.error("reader creation failed: \(error)")
            }
        }
    }

    // MARK: - Display link
    //
    // The obvious alternative was tried and measured worse, so it is written
    // down rather than left to be rediscovered: driving the pump from
    // `requestMediaDataWhenReady` with an `AVSampleBufferRenderSynchronizer`
    // timebase removes the display link, the main-thread wake per frame and both
    // dispatch hops — and cost 28 MB against 19 MB for no CPU improvement
    // (3.77% vs 3.58% on the same clip). The renderer reads ahead by an amount
    // we do not control, and at these resolutions one 10-bit frame is ~20 MB, so
    // buffering even a couple of them outweighs the whole app's footprint.
    // One frame in flight stays the right design.

    private func startDisplayLink() {
        stopDisplayLink()
        guard let screen else { return }

        let link = screen.displayLink(target: self, selector: #selector(tick))
        // Cap the tick rate at the asset's frame rate. A 24 fps ambient loop has
        // no reason to wake the CPU 120 times a second.
        // A pinned min==max range is unsatisfiable when the asset's rate isn't a
        // divisor of the display's (24 fps on a 60 Hz panel). Giving the range
        // slack lets CoreAnimation pick the nearest cadence it can actually hit
        // instead of falling back to something unpredictable.
        let fps = Float(targetFPS)
        link.preferredFrameRateRange = CAFrameRateRange(minimum: fps / 2, maximum: fps, preferred: fps)
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    private func stopDisplayLink() {
        displayLink?.invalidate()
        displayLink = nil
    }

    func attach(to screen: NSScreen) {
        self.screen = screen
        if isActive {
            startDisplayLink()
        }
    }

    @objc private func tick() {
        guard isActive, !decodeInFlight else { return }
        ticks += 1
        if ticks == 1 { Log.info("display link firing") }
        let renderer = displayLayer.sampleBufferRenderer

        // Deliberately not gated on `isReadyForMoreMediaData`: that flag only
        // becomes meaningful once `requestMediaDataWhenReady` is driving the
        // renderer, and this source paces itself from the display link instead
        // precisely so it never builds a read-ahead queue. One frame per tick,
        // each marked DisplayImmediately, is self-limiting.
        if renderer.status == .failed {
            Log.error("renderer failed: \(String(describing: renderer.error)) — flushing")
            renderer.flush()
        }

        decodeInFlight = true
        decodeQueue.async { [weak self] in
            guard let self else { return }
            defer { DispatchQueue.main.async { self.decodeInFlight = false } }

            guard let output = self.trackOutput, let reader = self.reader else { return }

            if reader.status == .failed {
                Log.error("reader failed mid-playback: \(String(describing: reader.error))")
                DispatchQueue.main.async { self.restartLoop() }
                return
            }

            guard let sample = output.copyNextSampleBuffer() else {
                // End of asset — seamless restart from the top.
                DispatchQueue.main.async { self.restartLoop() }
                return
            }

            // Track position so a teardown/resume cycle continues where it left off.
            let pts = CMSampleBufferGetPresentationTimeStamp(sample)
            if pts.isValid && pts.isNumeric {
                self.resumeTime = pts
            }

            Self.markDisplayImmediately(sample)
            renderer.enqueue(sample)

            self.framesEnqueued += 1
            if self.framesEnqueued % 120 == 0 {
                Log.info("frames=\(self.framesEnqueued) pts=\(String(format: "%.1f", CMTimeGetSeconds(pts)))s "
                         + "status=\(renderer.status.rawValue) \(Footprint.formatted())")
            }
        }
    }

    private func restartLoop() {
        resumeTime = .zero
        guard isActive else { return }
        if displayLayer.sampleBufferRenderer.status == .failed {
            displayLayer.sampleBufferRenderer.flush()
        }
        startReader(at: .zero)
    }

    /// Without a control timebase the renderer honours this attachment and
    /// presents the frame on the next composite — which is what lets the
    /// display link, rather than a media clock, own the pacing.
    private static func markDisplayImmediately(_ sample: CMSampleBuffer) {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true)
        else { return }
        let count = CFArrayGetCount(attachments)
        guard count > 0 else { return }
        let raw = CFArrayGetValueAtIndex(attachments, 0)
        let dict = unsafeBitCast(raw, to: CFMutableDictionary.self)
        CFDictionarySetValue(dict,
                             Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                             Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
    }
}
