import CoreGraphics
import Testing
@testable import LiveWall

/// Frame rate is the only import setting that costs CPU linearly at playback,
/// and output size is the one that decides whether the wallpaper looks upscaled.
/// Both are pure functions of the source and the display, so both are testable
/// without touching AVFoundation.
struct TranscoderTests {

    // MARK: - Frame pacing

    /// A frame that arrives between two refreshes is decoded and then dropped by
    /// the compositor, so the import rate should divide the panel's rate.

    @Test func ratesThatAlreadyDivideAreLeftAlone() {
        #expect(Transcoder.pacedFPS(preferred: 24, refresh: 120) == 24)
        #expect(Transcoder.pacedFPS(preferred: 30, refresh: 120) == 30)
        #expect(Transcoder.pacedFPS(preferred: 20, refresh: 60) == 20)
    }

    /// 60/24 is 2.5. Snapping down to 20 is both judder-free and cheaper than
    /// rounding up would be.
    @Test func awkwardRateSnapsDownOnASixtyHertzPanel() {
        #expect(Transcoder.pacedFPS(preferred: 24, refresh: 60) == 20)
    }

    @Test func snapsToTheNearestDivisorAtOrBelowThePreferredRate() {
        #expect(Transcoder.pacedFPS(preferred: 30, refresh: 144) == 24)
        #expect(Transcoder.pacedFPS(preferred: 13, refresh: 120) == 12)
    }

    /// Snapping below 12 fps would be a worse cure than the judder.
    @Test func doesNotSnapBelowTheFloor() {
        #expect(Transcoder.pacedFPS(preferred: 11, refresh: 120) == 11)
    }

    @Test func unknownRefreshRateChangesNothing() {
        #expect(Transcoder.pacedFPS(preferred: 24, refresh: 0) == 24)
        #expect(Transcoder.pacedFPS(preferred: 0, refresh: 120) == 1)
    }

    // MARK: - Output size

    private let panel = Transcoder.DisplayTarget(
        pixelSize: CGSize(width: 3024, height: 1964), maximumFramesPerSecond: 120)

    /// The `Native` preset sizes so the frame covers the panel — anything
    /// smaller is upscaled at playback, which was the single biggest quality
    /// loss in the pipeline.
    @Test func nativePresetCoversThePanel() {
        let size = Transcoder.outputSize(for: CGSize(width: 3840, height: 2160),
                                         preset: .native, display: panel)
        #expect(size == CGSize(width: 3492, height: 1964))
    }

    /// Inventing pixels the source never had costs memory and buys nothing.
    @Test func nativePresetNeverUpscalesPastTheSource() {
        let size = Transcoder.outputSize(for: CGSize(width: 1920, height: 1080),
                                         preset: .native, display: panel)
        #expect(size == CGSize(width: 1920, height: 1080))
    }

    @Test func fixedPresetFitsInsideItsLongestEdge() {
        let size = Transcoder.outputSize(for: CGSize(width: 3840, height: 2160),
                                         preset: .balanced, display: panel)
        #expect(size == CGSize(width: 1920, height: 1080))
    }

    @Test func fixedPresetLeavesSmallerSourcesAlone() {
        let size = Transcoder.outputSize(for: CGSize(width: 1280, height: 720),
                                         preset: .balanced, display: panel)
        #expect(size == CGSize(width: 1280, height: 720))
    }

    /// HEVC 4:2:0 cannot encode odd dimensions.
    @Test func dimensionsAreAlwaysEven() {
        let odd = CGSize(width: 1001, height: 667)
        for preset in Transcoder.Preset.all {
            let size = Transcoder.outputSize(for: odd, preset: preset, display: panel)
            #expect(Int(size.width) % 2 == 0)
            #expect(Int(size.height) % 2 == 0)
        }
    }

    @Test func portraitSourceIsHandled() {
        let size = Transcoder.outputSize(for: CGSize(width: 1080, height: 1920),
                                         preset: .balanced, display: panel)
        #expect(size == CGSize(width: 1080, height: 1920))
    }

    @Test func zeroSizedSourceDoesNotCrash() {
        let size = Transcoder.outputSize(for: .zero, preset: .balanced, display: panel)
        #expect(size.width > 0 && size.height > 0)
    }

    // MARK: - Presets

    /// Frame rate is the expensive knob; nothing should quietly raise it.
    @Test func noPresetExceedsThirtyFramesPerSecond() {
        for preset in Transcoder.Preset.all {
            #expect(preset.fps <= 30)
        }
    }
}
