import AppKit
import Foundation
import AVFoundation
import VideoToolbox

/// Normalises an arbitrary user video into a wallpaper-shaped asset.
///
/// Import is mandatory, not an optimisation pass — the playback path only ever
/// sees files that came out of here. That is what makes the runtime numbers
/// predictable regardless of what the user dragged in.
///
/// What each step is actually buying:
///
/// - **Transcode to HEVC.** VP9, AV1 and ProRes sources fall back to software
///   decode and cost tens of percent of a core. HEVC goes through the media
///   engine at roughly 1%.
/// - **Downscale.** A 4K source on a 1440p display decodes four times the pixels
///   nobody sees. Frame memory scales with this directly.
/// - **Cap the frame rate.** Ambient loops gain nothing above 24 fps and pay
///   linearly for every frame above it.
/// - **Drop the audio track.** No audio track means no audio graph at playback.
/// - **Disable frame reordering.** No B-frames means the decoder needs no
///   reorder buffer, which both shrinks its pool and removes decode latency.
/// - **`shouldOptimizeForNetworkUse`.** Moves the `moov` atom to the front so
///   opening the file doesn't read to the end first.
enum Transcoder {

    /// Measured on an M3 Pro against the real playback path: going from 1280x720
    /// to 3492x1964 (7.4x the pixels) moved the footprint 15 MB -> 18 MB and left
    /// CPU inside the noise, while going from 24 to 60 fps took CPU from 3.5% to
    /// 7.8%. HEVC decode is fixed-function on the media engine and flat in frame
    /// size; the CPU that remains is our own per-frame pump work, which scales
    /// with frames per second and nothing else.
    ///
    /// So the presets spend freely on resolution, bit depth and bitrate — all
    /// close to free at playback — and stay frugal with frame rate, which is the
    /// only knob that costs.
    struct Preset {
        let name: String
        /// Longest output edge in pixels, or `nil` to size to the display.
        let maxEdge: CGFloat?
        let fps: Int
        /// Bits per pixel per second. Costs disk and nothing else: the 6.8 Mbps
        /// variant measured marginally *cheaper* to play than the 1.4 Mbps one.
        /// The old 0.07 was starving the smooth gradient content that makes good
        /// wallpaper, which is what banding in smoke and glow looks like.
        let bitsPerPixel: Double
        /// 10-bit costs nothing to decode and is the real fix for banding.
        let bitDepth: Int
        /// Keyframes are expensive at these bitrates and a wallpaper seeks only
        /// when it resumes from occlusion, so they can be sparse.
        let keyframeSeconds: Double

        static let ultraLight = Preset(name: "Ultra Light", maxEdge: 960, fps: 20,
                                       bitsPerPixel: 0.10, bitDepth: 8, keyframeSeconds: 5)
        static let balanced   = Preset(name: "Balanced", maxEdge: 1920, fps: 24,
                                       bitsPerPixel: 0.15, bitDepth: 8, keyframeSeconds: 5)
        /// 24 rather than 30: frame rate is the one knob that costs CPU linearly
        /// (3.5% at 24 against 7.8% at 60, measured), it divides a 120 Hz panel
        /// as evenly as 30 does, and ambient loops gain nothing from the extra
        /// six frames.
        static let native     = Preset(name: "Native", maxEdge: nil, fps: 24,
                                       bitsPerPixel: 0.12, bitDepth: 10, keyframeSeconds: 5)

        static let all: [Preset] = [.ultraLight, .balanced, .native]

        /// Menu label. `maxEdge: nil` has no fixed number to show.
        var summary: String {
            let size = maxEdge.map { "\(Int($0))p" } ?? "your display"
            return "\(size) · \(fps) fps · \(bitDepth)-bit"
        }
    }

    /// What the output should be sized and paced against. Sourced from the
    /// screen the wallpaper will actually play on.
    struct DisplayTarget {
        /// Backing pixels, not points — a 1512x982 Retina panel is 3024x1964.
        let pixelSize: CGSize
        let maximumFramesPerSecond: Int

        static let fallback = DisplayTarget(pixelSize: CGSize(width: 1920, height: 1080),
                                            maximumFramesPerSecond: 60)

        @MainActor
        static func main() -> DisplayTarget {
            guard let screen = NSScreen.main else { return .fallback }
            return DisplayTarget(
                pixelSize: CGSize(width: screen.frame.width * screen.backingScaleFactor,
                                  height: screen.frame.height * screen.backingScaleFactor),
                maximumFramesPerSecond: max(1, screen.maximumFramesPerSecond))
        }
    }

    enum TranscodeError: LocalizedError {
        case noVideoTrack
        case unreadable(String)
        case writerSetupFailed(String)
        case failed(String)
        case cancelled

        var errorDescription: String? {
            switch self {
            case .noVideoTrack: return "That file has no video track."
            case .unreadable(let name):
                // AVFoundation's own message here is "Cannot Open" or "The
                // operation could not be completed", which tells the user
                // nothing actionable.
                return "Couldn't read “\(name)”. It may be missing, damaged, or not a video file."
            case .writerSetupFailed(let detail): return "Could not set up the encoder: \(detail)"
            case .failed(let detail): return "Conversion failed: \(detail)"
            case .cancelled: return "Conversion cancelled."
            }
        }
    }

    /// The reader, writer and pacer the conversion loop drives.
    ///
    /// None of AVFoundation's reader/writer types are `Sendable`, and the loop
    /// runs inside a `@Sendable` callback, so capturing them individually warns
    /// five times over. Grouping them states the actual invariant once:
    /// `requestMediaDataWhenReady` calls back on one serial queue and nothing
    /// else touches these objects for the duration, which is precisely the
    /// discipline AVFoundation asks for.
    private final class Pump: @unchecked Sendable {
        let reader: AVAssetReader
        let readerOutput: AVAssetReaderVideoCompositionOutput
        let writer: AVAssetWriter
        let writerInput: AVAssetWriterInput
        let pacer: FramePacer

        init(reader: AVAssetReader,
             readerOutput: AVAssetReaderVideoCompositionOutput,
             writer: AVAssetWriter,
             writerInput: AVAssetWriterInput,
             pacer: FramePacer) {
            self.reader = reader
            self.readerOutput = readerOutput
            self.writer = writer
            self.writerInput = writerInput
            self.pacer = pacer
        }
    }

    struct Result {
        let url: URL
        let size: CGSize
        let fps: Int
        let bitDepth: Int
        let byteCount: Int64
    }

    /// Converts `source` into `destination`. `progress` is called on the main
    /// queue with 0...1.
    static func convert(source: URL,
                        destination: URL,
                        preset: Preset,
                        display: DisplayTarget = .fallback,
                        options: ImportOptions = .default,
                        progress: @escaping (Double) -> Void) async throws -> Result {

        let asset = AVURLAsset(url: source, options: [
            AVURLAssetPreferPreciseDurationAndTimingKey: true
        ])

        let track: AVAssetTrack
        let duration: CMTime
        let naturalSize: CGSize
        let transform: CGAffineTransform
        let sourceFPS: Float

        do {
            guard let first = try await asset.loadTracks(withMediaType: .video).first else {
                throw TranscodeError.noVideoTrack
            }
            track = first
            duration = try await asset.load(.duration)
            naturalSize = try await track.load(.naturalSize)
            transform = try await track.load(.preferredTransform)
            sourceFPS = try await track.load(.nominalFrameRate)
        } catch let error as TranscodeError {
            throw error
        } catch {
            Log.error("asset load failed for \(source.lastPathComponent): \(error)")
            throw TranscodeError.unreadable(source.lastPathComponent)
        }

        // The track's own orientation plus whatever quarter turn the user asked
        // for. Everything downstream works in this one transform: the rotation
        // is baked into the output rather than recorded as a flag, so the
        // playback path never has to know a video was turned.
        let oriented = transform.concatenating(options.rotationTransform)

        // Orientation-corrected source dimensions. Transforming the *rect*
        // rather than the size keeps the origin a rotation moves off-screen,
        // which the fit transform below has to cancel out — and that already
        // holds for the user's turn, since it is folded into the same transform.
        let orientedRect = CGRect(origin: .zero, size: naturalSize).applying(oriented)
        let sourceSize = CGSize(width: abs(orientedRect.width), height: abs(orientedRect.height))
        let outputSize = outputSize(for: sourceSize, preset: preset, display: display)

        // Never upsample the frame rate past the source, and land on a rate the
        // display can actually present evenly: a frame that arrives between two
        // refreshes is decoded and then dropped by the compositor, which is the
        // most wasteful thing this pipeline can do. 24 divides 120 exactly; on a
        // 60 Hz panel it does not, and 20 is both smoother and cheaper.
        //
        // The user's chosen rate is the input to both rules rather than an
        // exemption from either.
        let preferred = options.preferredFPS(for: preset)
        let capped = sourceFPS > 0 ? min(preferred, Int(sourceFPS.rounded())) : preferred
        if capped != preferred {
            Log.info("capped \(preferred) fps to the source's \(capped)")
        }
        let fps = max(1, pacedFPS(preferred: capped, refresh: display.maximumFramesPerSecond))
        if fps != capped {
            Log.info("paced \(capped) fps down to \(fps) to divide a "
                     + "\(display.maximumFramesPerSecond) Hz display evenly")
        }

        // Built from the asset's properties for the colour metadata, then
        // re-instructed below.
        let composition = try await AVMutableVideoComposition.videoComposition(withPropertiesOf: asset)
        composition.renderSize = outputSize
        composition.frameDuration = CMTime(value: 1, timescale: CMTimeScale(fps))

        // `videoComposition(withPropertiesOf:)` builds layer instructions that
        // draw the track 1:1 into a canvas the size of the source. Shrinking
        // `renderSize` afterwards does not rescale them — the frame keeps its
        // original pixel dimensions and everything past the smaller canvas is
        // simply cut off, so a 1920x1080 source became the top-left 1280x720 of
        // itself. The downscale has to be expressed as a transform on the layer
        // instruction, which means replacing the generated instructions.
        composition.instructions = [fitInstruction(for: track,
                                                   duration: duration,
                                                   transform: oriented,
                                                   orientedRect: orientedRect,
                                                   outputSize: outputSize)]

        try? FileManager.default.removeItem(at: destination)

        let reader = try AVAssetReader(asset: asset)

        // Encoding to 10-bit is pointless if the composition hands the encoder
        // frames it has already crushed to 8-bit, so ask for a 10-bit
        // intermediate first and fall back to BGRA if the renderer won't do it.
        // BGRA is the format the composition renderer is happiest in, and this
        // is a one-time import cost on already-downscaled frames — neither
        // format reaches the runtime path.
        var readerOutput: AVAssetReaderVideoCompositionOutput?
        var intermediateFormats: [OSType] = [kCVPixelFormatType_32BGRA]
        if preset.bitDepth >= 10 {
            intermediateFormats.insert(kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, at: 0)
        }

        for format in intermediateFormats {
            let candidate = AVAssetReaderVideoCompositionOutput(
                videoTracks: [track],
                videoSettings: [kCVPixelBufferPixelFormatTypeKey as String: Int(format)])
            candidate.videoComposition = composition
            candidate.alwaysCopiesSampleData = false
            if reader.canAdd(candidate) {
                readerOutput = candidate
                if format != intermediateFormats.last {
                    Log.info("using a 10-bit composition intermediate")
                }
                break
            }
        }

        guard let readerOutput else {
            throw TranscodeError.writerSetupFailed("reader rejected composition output")
        }
        reader.add(readerOutput)

        let writer = try AVAssetWriter(outputURL: destination, fileType: .mov)
        writer.shouldOptimizeForNetworkUse = true

        let pixels = Double(outputSize.width * outputSize.height)
        let bitrate = Int(pixels * Double(fps) * preset.bitsPerPixel)

        let profile = preset.bitDepth >= 10
            ? kVTProfileLevel_HEVC_Main10_AutoLevel as String
            : kVTProfileLevel_HEVC_Main_AutoLevel as String

        let writerInput = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.hevc,
            AVVideoWidthKey: Int(outputSize.width),
            AVVideoHeightKey: Int(outputSize.height),
            AVVideoCompressionPropertiesKey: [
                AVVideoAverageBitRateKey: bitrate,
                AVVideoExpectedSourceFrameRateKey: fps,
                // Sparse keyframes: at these bitrates an I-frame every two
                // seconds ate a large share of the budget that the moving
                // content wanted. The only seek a wallpaper performs is the
                // resume after occlusion, which tolerates a longer run-up.
                AVVideoMaxKeyFrameIntervalKey: Int(Double(fps) * preset.keyframeSeconds),
                AVVideoMaxKeyFrameIntervalDurationKey: preset.keyframeSeconds,
                // No B-frames: the decoder needs no reorder buffer at playback.
                AVVideoAllowFrameReorderingKey: false,
                AVVideoProfileLevelKey: profile
            ]
        ])
        writerInput.expectsMediaDataInRealTime = false

        guard writer.canAdd(writerInput) else {
            throw TranscodeError.writerSetupFailed("writer rejected HEVC input")
        }
        writer.add(writerInput)

        guard reader.startReading() else {
            throw TranscodeError.failed(reader.error?.localizedDescription ?? "reader would not start")
        }
        guard writer.startWriting() else {
            throw TranscodeError.failed(writer.error?.localizedDescription ?? "writer would not start")
        }
        writer.startSession(atSourceTime: .zero)

        let totalSeconds = max(CMTimeGetSeconds(duration), 0.001)
        let queue = DispatchQueue(label: "livewall.transcode", qos: .userInitiated)

        // Setting `videoComposition.frameDuration` resizes frames but does not
        // retime them — the composition output still emits at the source
        // cadence. The frame grid has to be enforced here, by dropping source
        // frames that fall between grid points and stamping the survivors onto
        // exact 1/fps boundaries.
        //
        // This matters beyond tidiness: the playback path pulls exactly one
        // frame per display-link tick at the rate recorded in the library, so a
        // file whose real rate differs from its recorded rate plays at the
        // wrong speed.
        let pump = Pump(reader: reader, readerOutput: readerOutput,
                        writer: writer, writerInput: writerInput, pacer: FramePacer(fps: fps))

        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            writerInput.requestMediaDataWhenReady(on: queue) {
                while pump.writerInput.isReadyForMoreMediaData {
                    guard pump.reader.status == .reading else {
                        pump.writerInput.markAsFinished()
                        if pump.reader.status == .failed {
                            let message = pump.reader.error?.localizedDescription ?? "read failed"
                            pump.writer.cancelWriting()
                            continuation.resume(throwing: TranscodeError.failed(message))
                        } else {
                            continuation.resume()
                        }
                        return
                    }

                    guard let sample = pump.readerOutput.copyNextSampleBuffer() else {
                        pump.writerInput.markAsFinished()
                        continuation.resume()
                        return
                    }

                    let pts = CMSampleBufferGetPresentationTimeStamp(sample)

                    if let retimed = pump.pacer.accept(sample) {
                        if !pump.writerInput.append(retimed) {
                            pump.writerInput.markAsFinished()
                            pump.reader.cancelReading()
                            let message = pump.writer.error?.localizedDescription
                                ?? "encoder rejected a frame"
                            pump.writer.cancelWriting()
                            continuation.resume(throwing: TranscodeError.failed(message))
                            return
                        }
                    }

                    let fraction = min(max(CMTimeGetSeconds(pts) / totalSeconds, 0), 1)
                    DispatchQueue.main.async { progress(fraction) }
                }
            }
        }

        await writer.finishWriting()

        if writer.status == .failed {
            throw TranscodeError.failed(writer.error?.localizedDescription ?? "unknown encoder error")
        }

        let attributes = try? FileManager.default.attributesOfItem(atPath: destination.path)
        let bytes = (attributes?[.size] as? NSNumber)?.int64Value ?? 0
        DispatchQueue.main.async { progress(1) }

        let turn = ImportOptions.normalised(options.rotationDegrees)
        Log.info("converted to \(Int(outputSize.width))x\(Int(outputSize.height)) @ \(fps)fps "
                 + "\(preset.bitDepth)-bit"
                 + (turn != 0 ? " rotated \(turn)°" : "")
                 + ", \(bytes / 1024) KB")

        return Result(url: destination, size: outputSize, fps: fps,
                      bitDepth: preset.bitDepth, byteCount: bytes)
    }

    /// Largest frame rate no greater than `preferred` that divides `refresh`
    /// exactly. Falls back to `preferred` when nothing sensible divides it.
    static func pacedFPS(preferred: Int, refresh: Int) -> Int {
        guard preferred > 0, refresh > 0 else { return max(1, preferred) }
        var candidate = min(preferred, refresh)
        // 12 fps is the floor worth snapping to; below that the cure is worse
        // than the judder.
        while candidate >= 12 {
            if refresh % candidate == 0 { return candidate }
            candidate -= 1
        }
        return preferred
    }

    /// One instruction covering the whole asset, drawing the track orientation-
    /// corrected, uniformly scaled to fit `outputSize`, and centred.
    ///
    /// The three concatenated steps, in order:
    ///   1. `transform` — the track's own orientation fix.
    ///   2. `-orientedRect.origin` — a rotation transform maps the frame into
    ///      negative coordinates; without this the content sits off-canvas.
    ///   3. uniform scale, then centring for the sub-pixel slack left by
    ///      `targetSize`'s rounding to even dimensions.
    private static func fitInstruction(for track: AVAssetTrack,
                                       duration: CMTime,
                                       transform: CGAffineTransform,
                                       orientedRect: CGRect,
                                       outputSize: CGSize) -> AVMutableVideoCompositionInstruction {

        let sourceSize = CGSize(width: abs(orientedRect.width), height: abs(orientedRect.height))
        let scale = sourceSize.width > 0 && sourceSize.height > 0
            ? min(outputSize.width / sourceSize.width, outputSize.height / sourceSize.height)
            : 1.0
        let dx = (outputSize.width - sourceSize.width * scale) / 2
        let dy = (outputSize.height - sourceSize.height * scale) / 2

        let fitted = transform
            .concatenating(CGAffineTransform(translationX: -orientedRect.origin.x,
                                             y: -orientedRect.origin.y))
            .concatenating(CGAffineTransform(scaleX: scale, y: scale))
            .concatenating(CGAffineTransform(translationX: dx, y: dy))

        let layer = AVMutableVideoCompositionLayerInstruction(assetTrack: track)
        layer.setTransform(fitted, at: .zero)

        let instruction = AVMutableVideoCompositionInstruction()
        instruction.timeRange = CMTimeRange(start: .zero, duration: duration)
        instruction.layerInstructions = [layer]
        return instruction
    }

    /// Output dimensions for a preset.
    ///
    /// A fixed `maxEdge` fits the source inside that edge. `nil` sizes to the
    /// display instead — scaled so the frame *covers* the panel, since anything
    /// less is upscaled at playback and that upscale was the single largest
    /// quality loss in the pipeline. Neither path ever scales above 1:1: the
    /// source has no detail past its own resolution and inventing pixels only
    /// costs memory.
    static func outputSize(for source: CGSize,
                           preset: Preset,
                           display: DisplayTarget) -> CGSize {
        guard source.width > 0, source.height > 0 else {
            let edge = preset.maxEdge ?? display.pixelSize.width
            return even(CGSize(width: edge, height: edge * 9 / 16))
        }

        let scale: CGFloat
        if let maxEdge = preset.maxEdge {
            let longest = max(source.width, source.height)
            scale = longest > maxEdge ? maxEdge / longest : 1.0
        } else {
            let cover = max(display.pixelSize.width / source.width,
                            display.pixelSize.height / source.height)
            scale = min(cover, 1.0)
        }

        return even(CGSize(width: source.width * scale, height: source.height * scale))
    }

    /// HEVC 4:2:0 requires even dimensions.
    private static func even(_ size: CGSize) -> CGSize {
        let width = size.width.rounded()
        let height = size.height.rounded()
        return CGSize(width: max(2, width - width.truncatingRemainder(dividingBy: 2)),
                      height: max(2, height - height.truncatingRemainder(dividingBy: 2)))
    }
}
