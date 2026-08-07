// Frame pacing and frame-rate snapping. Mirrors the pacing half of
// TranscoderTests.swift.

#include "TestHarness.h"

#include "import/FramePacer.h"
#include "import/Transcoder.h"

using namespace livewall;

namespace {

constexpr long long kSecond = FramePacer::kHnsPerSecond;

}  // namespace

TEST_CASE("a source already at the target rate keeps every frame") {
    FramePacer pacer(24);
    for (int i = 0; i < 24; ++i) {
        const auto decision = pacer.accept(i * kSecond / 24);
        CHECK(decision.keep);
    }
    CHECK_EQ(pacer.kept(), 24);
    CHECK_EQ(pacer.dropped(), 0);
}

TEST_CASE("60 fps into a 24 fps grid keeps 24 frames a second") {
    FramePacer pacer(24);
    for (int i = 0; i < 60; ++i) pacer.accept(i * kSecond / 60);

    // 60 frames spread over one second onto a 1/24 grid: 24 grid points fall
    // inside that second.
    CHECK_EQ(pacer.kept(), 24);
    CHECK_EQ(pacer.kept() + pacer.dropped(), 60);
}

TEST_CASE("kept frames are stamped on an exact grid starting at zero") {
    FramePacer pacer(25);
    const auto first = pacer.accept(0);
    CHECK(first.keep);
    CHECK_EQ(first.outputTimeHns, 0LL);
    CHECK_EQ(first.durationHns, kSecond / 25);

    const auto second = pacer.accept(kSecond / 25);
    CHECK(second.keep);
    CHECK_EQ(second.outputTimeHns, kSecond / 25);

    const auto third = pacer.accept(2 * kSecond / 25);
    CHECK(third.keep);
    CHECK_EQ(third.outputTimeHns, 2 * kSecond / 25);
}

TEST_CASE("a source slower than the grid never emits the same instant twice") {
    // 10 fps into a 24 fps grid. Every source frame is kept, and each gets its
    // own output time — the cursor advance is what prevents two frames landing
    // on the same stamp.
    FramePacer pacer(24);
    long long previous = -1;
    for (int i = 0; i < 10; ++i) {
        const auto decision = pacer.accept(i * kSecond / 10);
        CHECK(decision.keep);
        CHECK(decision.outputTimeHns > previous);
        previous = decision.outputTimeHns;
    }
    CHECK_EQ(pacer.kept(), 10);
}

TEST_CASE("a frame arriving out of order is dropped rather than rewinding the grid") {
    FramePacer pacer(24);
    CHECK(pacer.accept(kSecond).keep);
    const auto backwards = pacer.accept(kSecond / 2);
    CHECK(!backwards.keep);
    CHECK_EQ(pacer.dropped(), 1);
}

TEST_CASE("a negative timestamp is dropped") {
    FramePacer pacer(24);
    CHECK(!pacer.accept(-1).keep);
    CHECK_EQ(pacer.kept(), 0);
}

TEST_CASE("a zero or negative frame rate does not divide by zero") {
    FramePacer pacer(0);
    CHECK(pacer.frameDurationHns() > 0);
}

// ---------------------------------------------------------------------------
// Frame-rate snapping
// ---------------------------------------------------------------------------

TEST_CASE("24 fps divides 120 Hz exactly and is left alone") {
    CHECK_EQ(Transcoder::pacedFPS(24, 120), 24);
}

TEST_CASE("24 fps on a 60 Hz panel snaps down to 20") {
    // 24 does not divide 60; 20 does, and is both smoother and cheaper than
    // letting every fifth frame arrive between two refreshes and be dropped.
    CHECK_EQ(Transcoder::pacedFPS(24, 60), 20);
}

TEST_CASE("30 fps divides 60 Hz exactly") {
    CHECK_EQ(Transcoder::pacedFPS(30, 60), 30);
}

TEST_CASE("24 fps on a 144 Hz panel snaps to 24") {
    CHECK_EQ(Transcoder::pacedFPS(24, 144), 24);
}

TEST_CASE("a request above the refresh rate is capped by it") {
    CHECK_EQ(Transcoder::pacedFPS(60, 30), 30);
}

TEST_CASE("nothing snaps below 12 fps") {
    // 13 Hz is absurd, and there is no divisor at or above 12 — the cure would
    // be worse than the judder, so the preferred rate is returned unchanged.
    CHECK_EQ(Transcoder::pacedFPS(24, 13), 24);
}

TEST_CASE("degenerate inputs return something playable") {
    CHECK_EQ(Transcoder::pacedFPS(0, 60), 1);
    CHECK_EQ(Transcoder::pacedFPS(24, 0), 24);
}
