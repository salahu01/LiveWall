import CoreGraphics
import Testing
@testable import LiveWall

/// The uncovered-desktop fraction decides whether the decoder runs at all, and
/// it is derived from geometry the tests can supply directly. `visibleFraction`
/// itself reads the live window list and can't be pinned down in a test; the
/// arithmetic underneath it is the part that can be wrong quietly.
struct DesktopVisibilityTests {

    private let screen = CGRect(x: 0, y: 0, width: 1000, height: 1000)

    /// The grid is 64x40 cells, so a boundary can land up to about a cell off.
    private let tolerance = 0.03

    @Test func bareDesktopIsFullyVisible() {
        #expect(DesktopVisibility.uncoveredFraction(of: screen, occludedBy: []) == 1.0)
    }

    @Test func fullScreenWindowLeavesNothing() {
        #expect(DesktopVisibility.uncoveredFraction(of: screen, occludedBy: [screen]) == 0.0)
    }

    @Test func halfCoveredReadsAsHalf() {
        let half = CGRect(x: 0, y: 0, width: 500, height: 1000)
        let got = DesktopVisibility.uncoveredFraction(of: screen, occludedBy: [half])
        #expect(abs(got - 0.5) <= tolerance)
    }

    /// Overlapping windows must not each subtract their own area — the naive
    /// sum-of-areas version of this would report a negative fraction.
    @Test func overlappingWindowsAreNotDoubleCounted() {
        let left = CGRect(x: 0, y: 0, width: 600, height: 1000)
        let right = CGRect(x: 400, y: 0, width: 600, height: 1000)
        #expect(DesktopVisibility.uncoveredFraction(of: screen, occludedBy: [left, right]) == 0.0)
    }

    /// The case the whole feature exists for: occlusion still calls this
    /// "visible", and we want to know it is 5%.
    @Test func nearlyCoveredIsDetected() {
        let almost = CGRect(x: 0, y: 0, width: 1000, height: 950)
        let got = DesktopVisibility.uncoveredFraction(of: screen, occludedBy: [almost])
        #expect(abs(got - 0.05) <= tolerance)
    }

    @Test func windowOnAnotherDisplayIsIgnored() {
        let elsewhere = CGRect(x: 2000, y: 2000, width: 500, height: 500)
        #expect(DesktopVisibility.uncoveredFraction(of: screen, occludedBy: [elsewhere]) == 1.0)
    }

    /// Windows routinely hang off the edge of a display; only the overlap counts.
    @Test func windowHangingOffTheEdgeCountsOnlyItsOverlap() {
        let hanging = CGRect(x: 750, y: 0, width: 1000, height: 1000)
        let got = DesktopVisibility.uncoveredFraction(of: screen, occludedBy: [hanging])
        #expect(abs(got - 0.75) <= tolerance)
    }

    @Test func cornerWindowCoversAQuarter() {
        let corner = CGRect(x: 0, y: 0, width: 500, height: 500)
        let got = DesktopVisibility.uncoveredFraction(of: screen, occludedBy: [corner])
        #expect(abs(got - 0.75) <= tolerance)
    }

    /// A second display has a non-zero origin in the global coordinate space.
    /// Getting this wrong would gate the wrong screen.
    @Test func secondaryDisplayOriginIsRespected() {
        let secondary = CGRect(x: 1512, y: 0, width: 1000, height: 1000)
        let onIt = CGRect(x: 1512, y: 0, width: 500, height: 1000)
        let got = DesktopVisibility.uncoveredFraction(of: secondary, occludedBy: [onIt])
        #expect(abs(got - 0.5) <= tolerance)
    }

    @Test func windowOnThePrimaryDoesNotOccludeTheSecondary() {
        let secondary = CGRect(x: 1512, y: 0, width: 1000, height: 1000)
        let onPrimary = CGRect(x: 0, y: 0, width: 1000, height: 1000)
        #expect(DesktopVisibility.uncoveredFraction(of: secondary, occludedBy: [onPrimary]) == 1.0)
    }

    @Test func degenerateDisplayDoesNotDivideByZero() {
        let empty = CGRect(x: 0, y: 0, width: 0, height: 0)
        #expect(DesktopVisibility.uncoveredFraction(of: empty, occludedBy: [screen]) == 1.0)
    }
}
