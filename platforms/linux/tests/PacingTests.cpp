// Frame pacing and the refresh-divisor arithmetic.
#include "Testing.h"

#include "import/FramePacer.h"
#include "import/Transcoder.h"

using namespace livewall;

namespace {

// Feeds `count` frames at `sourceFps` through a pacer targeting `targetFps` and
// returns how many survived.
int keptFrames(int sourceFps, int targetFps, int count) {
    FramePacer pacer(targetFps);
    for (int i = 0; i < count; ++i) {
        std::int64_t index = 0;
        pacer.accept(static_cast<std::int64_t>(i) * 1000000 / sourceFps, &index);
    }
    return pacer.kept();
}

}  // namespace

TEST(pacing, sameRatePassesEverything) {
    EXPECT_EQ(keptFrames(24, 24, 240), 240);
}

TEST(pacing, halfRateDropsHalf) {
    // 48 fps into a 24 fps grid. One second in, 24 frames should have survived.
    EXPECT_EQ(keptFrames(48, 24, 48), 24);
}

TEST(pacing, sixtyIntoTwentyFour) {
    // The commonest real case, and the one that is not a clean ratio: 60 fps
    // phone footage paced to 24.
    const int kept = keptFrames(60, 24, 600);
    // Ten seconds of source; 24 fps means 240 frames, give or take the one at
    // the boundary.
    EXPECT_TRUE(kept >= 239 && kept <= 241);
}

TEST(pacing, slowSourceNeverEmitsAFrameTwice) {
    // 12 fps into a 24 fps grid. The grid wants more frames than exist, and the
    // pacer must not stamp the same instant twice — which is what the loop
    // advancing past the source timestamp is for.
    FramePacer pacer(24);
    std::int64_t previous = -1;
    for (int i = 0; i < 24; ++i) {
        std::int64_t index = 0;
        if (!pacer.accept(static_cast<std::int64_t>(i) * 1000000 / 12, &index)) continue;
        EXPECT_TRUE(index > previous);
        previous = index;
    }
    EXPECT_EQ(pacer.kept(), 24);
}

TEST(pacing, outputIndicesAreConsecutiveFromZero) {
    FramePacer pacer(30);
    std::int64_t expected = 0;
    for (int i = 0; i < 120; ++i) {
        std::int64_t index = 0;
        if (!pacer.accept(static_cast<std::int64_t>(i) * 1000000 / 60, &index)) continue;
        EXPECT_EQ(index, expected);
        ++expected;
    }
    // A gap in the output timestamps would play as a stutter that no amount of
    // decoder tuning explains.
    EXPECT_EQ(pacer.kept(), static_cast<int>(expected));
}

TEST(pacing, keptAndDroppedAccountForEveryFrame) {
    FramePacer pacer(24);
    for (int i = 0; i < 300; ++i) {
        std::int64_t index = 0;
        pacer.accept(static_cast<std::int64_t>(i) * 1000000 / 60, &index);
    }
    EXPECT_EQ(pacer.kept() + pacer.dropped(), 300);
}

TEST(pacing, aZeroFpsRequestDoesNotDivideByZero) {
    FramePacer pacer(0);
    std::int64_t index = 0;
    EXPECT_TRUE(pacer.accept(0, &index));
    EXPECT_EQ(pacer.fps(), 1);
}

// --- the refresh-divisor arithmetic -----------------------------------------

TEST(paced_fps, twentyFourDividesOneHundredTwenty) {
    EXPECT_EQ(Transcoder::pacedFps(24, 120), 24);
}

TEST(paced_fps, twentyFourOnSixtyHertzBecomesTwenty) {
    // 60 % 24 != 0, so a frame lands between refreshes and is dropped by the
    // compositor. 20 divides 60 exactly and is both smoother and cheaper.
    EXPECT_EQ(Transcoder::pacedFps(24, 60), 20);
}

TEST(paced_fps, thirtyOnSixtyHertzStaysThirty) {
    EXPECT_EQ(Transcoder::pacedFps(30, 60), 30);
}

TEST(paced_fps, neverExceedsTheRefreshRate) {
    EXPECT_TRUE(Transcoder::pacedFps(60, 30) <= 30);
}

TEST(paced_fps, snapsToAnyDivisorDownToTwelve) {
    // 143 Hz is a real panel rate and factors as 11 x 13, so 13 is the largest
    // rate at or below 24 that divides it. Dropping 24 -> 13 to avoid dropped
    // frames is a large step, and it is the same answer the macOS port gives;
    // the alternative is presenting frames the compositor discards.
    EXPECT_EQ(Transcoder::pacedFps(24, 143), 13);
}

TEST(paced_fps, fallsBackWhenNothingSensibleDivides) {
    // 121 Hz factors as 11 x 11, so nothing between 12 and 24 divides it. The
    // floor of 12 is the point at which the cure is worse than the judder, and
    // below it the preferred rate is kept unchanged.
    EXPECT_EQ(Transcoder::pacedFps(24, 121), 24);
}

TEST(paced_fps, zeroesAreSurvivable) {
    EXPECT_EQ(Transcoder::pacedFps(24, 0), 24);
    EXPECT_EQ(Transcoder::pacedFps(0, 60), 1);
}
