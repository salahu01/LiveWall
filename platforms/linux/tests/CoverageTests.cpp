// The coverage geometry — the measurement the whole occlusion gate rests on.
//
// Same cases as the macOS suite's DesktopVisibilityTests, because the algorithm
// is the same one and a difference in either would be a bug in one of them.
#include "Testing.h"

#include "support/DesktopVisibility.h"

using namespace livewall;

namespace {

const Rect kDisplay = {0, 0, 1920, 1080};

double uncovered(const std::vector<Rect>& occluders, const Rect& bounds = kDisplay) {
    return DesktopVisibility::uncoveredFraction(bounds, occluders);
}

// The grid is 64x40, so a rectangle that does not land on a cell boundary
// rounds by up to half a cell on each edge. That is 1/128 of the width and
// 1/80 of the height, and every tolerance below is derived from it rather than
// guessed.
constexpr double kGridTolerance = 0.02;

}  // namespace

TEST(coverage, nothingCoversEverything) {
    EXPECT_NEAR(uncovered({}), 1.0, 1e-9);
}

TEST(coverage, oneFullScreenWindowCoversAll) {
    EXPECT_NEAR(uncovered({{0, 0, 1920, 1080}}), 0.0, 1e-9);
}

TEST(coverage, halfWidthWindowCoversHalf) {
    EXPECT_NEAR(uncovered({{0, 0, 960, 1080}}), 0.5, kGridTolerance);
}

TEST(coverage, quarterWindowCoversQuarter) {
    EXPECT_NEAR(uncovered({{0, 0, 960, 540}}), 0.75, kGridTolerance);
}

TEST(coverage, overlappingWindowsAreNotDoubleCounted) {
    // Two windows each covering half, overlapping in the middle quarter. A
    // rectangle-area sum would report more than the display.
    const double fraction = uncovered({{0, 0, 1152, 1080}, {768, 0, 1920, 1080}});
    EXPECT_NEAR(fraction, 0.0, kGridTolerance);
}

TEST(coverage, windowsOffTheEdgeAreClipped) {
    // Hanging half off the left. Only the on-screen half counts.
    EXPECT_NEAR(uncovered({{-960, 0, 960, 1080}}), 0.5, kGridTolerance);
}

TEST(coverage, windowEntirelyOffScreenCoversNothing) {
    EXPECT_NEAR(uncovered({{-4000, -4000, -2000, -2000}}), 1.0, 1e-9);
}

TEST(coverage, secondMonitorOffsetIsRespected) {
    // A display whose origin is not zero — the normal case for anything but
    // the primary monitor. A window at the origin of the *root* covers nothing
    // of it.
    const Rect second = {1920, 0, 3840, 1080};
    EXPECT_NEAR(uncovered({{0, 0, 1920, 1080}}, second), 1.0, 1e-9);
    EXPECT_NEAR(uncovered({{1920, 0, 3840, 1080}}, second), 0.0, 1e-9);
}

TEST(coverage, windowSpanningTwoMonitorsCountsOnEach) {
    const Rect second = {1920, 0, 3840, 1080};
    const std::vector<Rect> straddling = {{960, 0, 2880, 1080}};
    EXPECT_NEAR(uncovered(straddling, kDisplay), 0.5, kGridTolerance);
    EXPECT_NEAR(uncovered(straddling, second), 0.5, kGridTolerance);
}

TEST(coverage, degenerateBoundsReportFullyVisible) {
    // A zero-sized output cannot be covered, and dividing by its area would be
    // the alternative.
    EXPECT_NEAR(uncovered({{0, 0, 100, 100}}, Rect{0, 0, 0, 0}), 1.0, 1e-9);
    EXPECT_NEAR(uncovered({{0, 0, 100, 100}}, Rect{0, 0, 1920, 0}), 1.0, 1e-9);
}

TEST(coverage, degenerateOccludersCoverNothing) {
    EXPECT_NEAR(uncovered({{500, 500, 500, 900}}), 1.0, 1e-9);
    EXPECT_NEAR(uncovered({{500, 500, 900, 500}}), 1.0, 1e-9);
}

TEST(coverage, aSliverLeftUncoveredIsBelowTheStopThreshold) {
    // The case the whole hysteresis band exists for: a window covering all but
    // a strip down the right-hand side. The macOS port's occlusion state would
    // call this "visible" and decode at full rate for it.
    const double fraction = uncovered({{0, 0, 1850, 1080}});
    EXPECT_TRUE(fraction < 0.08);
    EXPECT_TRUE(fraction > 0.0);
}

TEST(coverage, aQuarterUncoveredIsAboveTheResumeThreshold) {
    EXPECT_TRUE(uncovered({{0, 0, 1440, 1080}}) > 0.15);
}
