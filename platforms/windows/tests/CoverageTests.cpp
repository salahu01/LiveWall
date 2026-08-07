// The desktop-coverage geometry. Mirrors DesktopVisibilityTests.swift.
//
// Only the pure half is exercised — `uncoveredFraction` — because the other
// half walks the live window list and a CI runner has no desktop. That split is
// exactly why the function was separated from the enumeration in the first
// place.

#include "TestHarness.h"

#include "app/MonitorController.h"
#include "support/DesktopVisibility.h"

using namespace livewall;

namespace {

Rect display() { return Rect{0, 0, 1920, 1080}; }

}  // namespace

TEST_CASE("an empty desktop is fully visible") {
    CHECK_NEAR(DesktopVisibility::uncoveredFraction(display(), {}), 1.0, 0.001);
}

TEST_CASE("a window covering the whole display leaves nothing visible") {
    CHECK_NEAR(DesktopVisibility::uncoveredFraction(display(), {display()}), 0.0, 0.001);
}

TEST_CASE("half a display covered reads as about half visible") {
    const std::vector<Rect> occluders{{0, 0, 960, 1080}};
    // The grid is 64x40, so a clean half falls exactly on cell boundaries.
    CHECK_NEAR(DesktopVisibility::uncoveredFraction(display(), occluders), 0.5, 0.01);
}

TEST_CASE("overlapping windows are not double counted") {
    const std::vector<Rect> occluders{{0, 0, 1000, 1080}, {500, 0, 1500, 1080}};
    // Union is 0..1500 of 1920, so about 22% remains.
    CHECK_NEAR(DesktopVisibility::uncoveredFraction(display(), occluders), 0.219, 0.02);
}

TEST_CASE("a window hanging off the edge only counts where it overlaps") {
    const std::vector<Rect> occluders{{-500, -500, 460, 1580}};
    // 0..460 of 1920 is covered, so about 76% remains.
    CHECK_NEAR(DesktopVisibility::uncoveredFraction(display(), occluders), 0.76, 0.02);
}

TEST_CASE("a window entirely off the display covers nothing") {
    const std::vector<Rect> occluders{{2000, 0, 3920, 1080}};
    CHECK_NEAR(DesktopVisibility::uncoveredFraction(display(), occluders), 1.0, 0.001);
}

TEST_CASE("a degenerate display reports fully visible rather than dividing by zero") {
    CHECK_NEAR(DesktopVisibility::uncoveredFraction(Rect{0, 0, 0, 0}, {display()}), 1.0, 0.001);
}

TEST_CASE("a display at a negative origin is handled") {
    // The left-hand monitor of a two-monitor setup has negative coordinates,
    // and the grid has to be laid out relative to the display's own origin
    // rather than to zero.
    const Rect secondary{-1920, 0, 0, 1080};
    const std::vector<Rect> occluders{{-1920, 0, -960, 1080}};
    CHECK_NEAR(DesktopVisibility::uncoveredFraction(secondary, occluders), 0.5, 0.01);
}

TEST_CASE("the hysteresis band has a gap, so an edge resting on it cannot oscillate") {
    // The two thresholds must not be equal: with one threshold, a window edge
    // sitting on it toggles the decoder every poll.
    CHECK(MonitorController::kStopBelowFraction < MonitorController::kResumeAboveFraction);
}

TEST_CASE("a nearly covered desktop lands below the stop threshold") {
    // 1830 of 1920 covered leaves under 5%, which is inside the stop band.
    const std::vector<Rect> occluders{{0, 0, 1830, 1080}};
    const double visible = DesktopVisibility::uncoveredFraction(display(), occluders);
    CHECK(visible < MonitorController::kStopBelowFraction);
}

TEST_CASE("a desktop with a fifth showing stays above the resume threshold") {
    const std::vector<Rect> occluders{{0, 0, 1500, 1080}};
    const double visible = DesktopVisibility::uncoveredFraction(display(), occluders);
    CHECK(visible > MonitorController::kResumeAboveFraction);
}
