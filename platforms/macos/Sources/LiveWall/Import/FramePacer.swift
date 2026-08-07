import Foundation
import CoreMedia

/// Resamples a stream of video frames onto an exact 1/fps grid.
///
/// Frames arriving between grid points are dropped; the ones that survive are
/// stamped with clean, evenly spaced presentation times starting at zero. That
/// gives the encoder a genuinely constant frame rate rather than the source
/// cadence with a frame-rate hint attached.
///
/// Not thread-safe: it is driven from the transcoder's single serial pump.
final class FramePacer {

    private let frameDuration: CMTime
    /// Next position on the source timeline we still want a frame for.
    private var nextSourceTime: CMTime = .zero
    /// Presentation time to stamp on the next frame we keep.
    private var outputTime: CMTime = .zero

    private(set) var kept = 0
    private(set) var dropped = 0

    init(fps: Int) {
        self.frameDuration = CMTime(value: 1, timescale: CMTimeScale(max(1, fps)))
    }

    /// Returns a retimed copy of `sample` if it lands on the grid, or nil if it
    /// should be dropped.
    func accept(_ sample: CMSampleBuffer) -> CMSampleBuffer? {
        let pts = CMSampleBufferGetPresentationTimeStamp(sample)
        guard pts.isValid && pts.isNumeric else { return nil }

        guard pts >= nextSourceTime else {
            dropped += 1
            return nil
        }

        var timing = CMSampleTimingInfo(duration: frameDuration,
                                        presentationTimeStamp: outputTime,
                                        decodeTimeStamp: .invalid)

        var retimed: CMSampleBuffer?
        let status = CMSampleBufferCreateCopyWithNewTiming(
            allocator: kCFAllocatorDefault,
            sampleBuffer: sample,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timing,
            sampleBufferOut: &retimed)

        guard status == noErr, let retimed else { return nil }

        kept += 1
        outputTime = outputTime + frameDuration

        // Advance the source cursor past `pts` so a source slower than the
        // target grid can't make us emit the same instant twice.
        repeat {
            nextSourceTime = nextSourceTime + frameDuration
        } while nextSourceTime <= pts

        return retimed
    }
}
