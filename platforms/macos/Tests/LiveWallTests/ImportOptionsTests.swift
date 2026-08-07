import CoreGraphics
import Testing
@testable import LiveWall

/// Frame rate and rotation are the two import settings that belong to one video
/// rather than to the library. Both resolve through pure functions of the
/// request, the source and the display, so both are testable without touching
/// AVFoundation.
struct ImportOptionsTests {

    // MARK: - Frame rate

    @Test func noOpinionFallsBackToThePresetRate() {
        #expect(ImportOptions().preferredFPS(for: .balanced) == 24)
        #expect(ImportOptions().preferredFPS(for: .ultraLight) == 20)
    }

    /// Below the preset as well as above it — the point is the user's number,
    /// not a raised ceiling.
    @Test func aChosenRateWinsOverThePreset() {
        #expect(ImportOptions(fps: 30).preferredFPS(for: .balanced) == 30)
        #expect(ImportOptions(fps: 15).preferredFPS(for: .native) == 15)
    }

    /// A typo in a free-entry field must not reach the encoder's bitrate maths.
    @Test func aRateOutsideTheOfferedRangeIsClampedNotRejected() {
        #expect(ImportOptions(fps: 9000).preferredFPS(for: .balanced) == ImportOptions.maximumFPS)
        #expect(ImportOptions(fps: 1).preferredFPS(for: .balanced) == ImportOptions.minimumFPS)
        #expect(ImportOptions(fps: -30).preferredFPS(for: .balanced) == ImportOptions.minimumFPS)
    }

    /// `pacedFPS` gives up below 12 and hands the request back unsnapped, so
    /// offering a rate under that would return the judder the rest of the
    /// pipeline exists to avoid.
    @Test func theFloorIsAtOrAboveThePacerSnappingFloor() {
        #expect(ImportOptions.minimumFPS >= 12)
    }

    /// The contract the picker promises: your number is the input to the pacing
    /// rule, not an exemption from it. 30 divides 90, but not 100.
    @Test func aChosenRateStillGoesThroughTheDisplaySnap() {
        let chosen = ImportOptions(fps: 30).preferredFPS(for: .balanced)
        #expect(Transcoder.pacedFPS(preferred: chosen, refresh: 90) == 30)
        #expect(Transcoder.pacedFPS(preferred: chosen, refresh: 100) == 25)
    }

    // MARK: - Rotation

    /// A clip already carrying a 90° `preferredTransform`, turned another 90° by
    /// the user, is a 180° clip — which is what pressing rotate twice looks like.
    @Test func rotationIsAdditiveOnTopOfTheTrackTransform() {
        let track = CGAffineTransform(rotationAngle: .pi / 2)
        let combined = track.concatenating(ImportOptions(rotationDegrees: 90).rotationTransform)
        let expected = CGAffineTransform(rotationAngle: .pi)

        #expect(abs(combined.a - expected.a) < 1e-9)
        #expect(abs(combined.b - expected.b) < 1e-9)
        #expect(abs(combined.c - expected.c) < 1e-9)
        #expect(abs(combined.d - expected.d) < 1e-9)
    }

    /// Which is what lets the transcoder concatenate unconditionally, with no
    /// special case for "no rotation".
    @Test func noRotationComposesAwayToNothing() {
        let track = CGAffineTransform(rotationAngle: .pi / 2)
        #expect(track.concatenating(ImportOptions.default.rotationTransform) == track)
        #expect(ImportOptions.default.rotationTransform == .identity)
    }

    /// The property the output sizing depends on: a 1920x1080 clip turned 90° is
    /// a portrait clip and has to be fitted to the portrait edge.
    @Test func aQuarterTurnSwapsTheOrientedRectAndAHalfTurnDoesNot() {
        let natural = CGRect(origin: .zero, size: CGSize(width: 1920, height: 1080))

        for degrees in [90, 270] {
            let turned = natural.applying(ImportOptions(rotationDegrees: degrees).rotationTransform)
            #expect(abs(abs(turned.width) - 1080) < 1e-6)
            #expect(abs(abs(turned.height) - 1920) < 1e-6)
        }

        for degrees in [0, 180] {
            let turned = natural.applying(ImportOptions(rotationDegrees: degrees).rotationTransform)
            #expect(abs(abs(turned.width) - 1920) < 1e-6)
            #expect(abs(abs(turned.height) - 1080) < 1e-6)
        }
    }

    @Test func swapsEdgesAgreesWithTheTransform() {
        #expect(!ImportOptions(rotationDegrees: 0).swapsEdges)
        #expect(ImportOptions(rotationDegrees: 90).swapsEdges)
        #expect(!ImportOptions(rotationDegrees: 180).swapsEdges)
        #expect(ImportOptions(rotationDegrees: 270).swapsEdges)
    }

    @Test func aNegativeOrOutOfRangeAngleDoesNotProduceANegativeResult() {
        #expect(ImportOptions.normalised(-90) == 270)
        #expect(ImportOptions.normalised(450) == 90)
        #expect(ImportOptions.normalised(360) == 0)
        #expect(ImportOptions.normalised(-360) == 0)
    }

    @Test func everyOfferedRotationIsAQuarterTurnInsideOneRevolution() {
        #expect(ImportOptions.rotations == [0, 90, 180, 270])
        for degrees in ImportOptions.rotations {
            #expect(degrees % 90 == 0)
            #expect(ImportOptions.normalised(degrees) == degrees)
        }
    }

    // MARK: - Defaults and sizing

    /// So an unchanged import still lands exactly where it did before.
    @Test func theDefaultAsksForNothing() {
        #expect(ImportOptions.default.fps == nil)
        #expect(ImportOptions.default.rotationDegrees == 0)
        #expect(ImportOptions.default.preferredFPS(for: .balanced) == Transcoder.Preset.balanced.fps)
    }

    /// The bug this guards: a 1920x1080 source turned 90° is portrait. Sizing it
    /// as landscape would fit 1920 to `maxEdge` and leave the real long edge —
    /// the 1080 that became the height — fitted to nothing.
    @Test func aRotatedClipIsSizedAgainstTheEdgeItActuallyHas() {
        let natural = CGRect(origin: .zero, size: CGSize(width: 1920, height: 1080))
        let turned = natural.applying(ImportOptions(rotationDegrees: 90).rotationTransform)
        let source = CGSize(width: abs(turned.width), height: abs(turned.height))

        let output = Transcoder.outputSize(for: source, preset: .ultraLight, display: .fallback)

        #expect(max(output.width, output.height) == 960)
        #expect(output.height > output.width)
    }
}
