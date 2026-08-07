// Import output sizing and the fit-mode arithmetic. Mirrors the sizing half of
// TranscoderTests.swift plus the FitMode logic.

#include "TestHarness.h"

#include "import/Transcoder.h"
#include "render/FitMode.h"

using namespace livewall;

namespace {

Transcoder::DisplayTarget retina() {
    return Transcoder::DisplayTarget{3024, 1964, 120};
}

Transcoder::DisplayTarget fullHd() {
    return Transcoder::DisplayTarget{1920, 1080, 60};
}

struct Size {
    int width = 0;
    int height = 0;
};

Size sizeFor(int sourceWidth, int sourceHeight, const Transcoder::Preset& preset,
             const Transcoder::DisplayTarget& display) {
    Size size;
    Transcoder::outputSize(sourceWidth, sourceHeight, preset, display, &size.width,
                           &size.height);
    return size;
}

}  // namespace

TEST_CASE("a fixed max edge fits the source inside it") {
    const Size size = sizeFor(3840, 2160, Transcoder::kBalanced, fullHd());
    CHECK_EQ(size.width, 1920);
    CHECK_EQ(size.height, 1080);
}

TEST_CASE("a source smaller than the max edge is never upscaled") {
    // Inventing pixels costs memory at playback and adds no detail the source
    // ever had.
    const Size size = sizeFor(1280, 720, Transcoder::kBalanced, fullHd());
    CHECK_EQ(size.width, 1280);
    CHECK_EQ(size.height, 720);
}

TEST_CASE("Ultra Light caps the long edge at 960") {
    const Size size = sizeFor(1920, 1080, Transcoder::kUltraLight, fullHd());
    CHECK_EQ(size.width, 960);
    CHECK_EQ(size.height, 540);
}

TEST_CASE("a portrait source is capped on its long edge, which is the height") {
    const Size size = sizeFor(1080, 1920, Transcoder::kUltraLight, fullHd());
    CHECK_EQ(size.height, 960);
    CHECK_EQ(size.width, 540);
}

TEST_CASE("Native scales a large source to cover the panel and no further") {
    // 3840x2160 on a 3024x1964 panel: covering needs 1964/2160 = 0.909 on
    // height and 3024/3840 = 0.7875 on width, so the larger wins.
    const Size size = sizeFor(3840, 2160, Transcoder::kNative, retina());
    CHECK(size.width >= retina().pixelWidth);
    CHECK(size.height >= retina().pixelHeight);
    // ...and no more than it needs to.
    CHECK(size.width < 3840);
}

TEST_CASE("Native never upscales past 1:1 even when the source is smaller than the panel") {
    const Size size = sizeFor(1280, 720, Transcoder::kNative, retina());
    CHECK_EQ(size.width, 1280);
    CHECK_EQ(size.height, 720);
}

TEST_CASE("output dimensions are always even") {
    // 4:2:0 chroma subsampling requires it, and every hardware encoder rejects
    // an odd dimension with an unhelpful error.
    const Size a = sizeFor(1919, 1079, Transcoder::kNative, fullHd());
    CHECK_EQ(a.width % 2, 0);
    CHECK_EQ(a.height % 2, 0);

    const Size b = sizeFor(999, 333, Transcoder::kUltraLight, fullHd());
    CHECK_EQ(b.width % 2, 0);
    CHECK_EQ(b.height % 2, 0);
}

TEST_CASE("a degenerate source size still produces a usable frame") {
    const Size size = sizeFor(0, 0, Transcoder::kBalanced, fullHd());
    CHECK(size.width >= 2);
    CHECK(size.height >= 2);
    CHECK_EQ(size.width % 2, 0);
}

TEST_CASE("preset names round-trip, including the retired one") {
    CHECK_EQ(std::string(Transcoder::presetByName("Native")->name), std::string("Native"));
    CHECK_EQ(std::string(Transcoder::presetByName("Ultra Light")->name),
             std::string("Ultra Light"));
    // "Fidelity" was the old top preset; anyone who chose it wanted the best
    // available, which is now Native.
    CHECK_EQ(std::string(Transcoder::presetByName("Fidelity")->name), std::string("Native"));
    // An unknown name falls back to Balanced rather than to nothing.
    CHECK_EQ(std::string(Transcoder::presetByName("nonsense")->name), std::string("Balanced"));
}

// ---------------------------------------------------------------------------
// Fit modes
// ---------------------------------------------------------------------------

TEST_CASE("matching aspect ratios need no scaling in any mode") {
    for (const FitMode mode : kAllFitModes) {
        const FitScale scale = fitScale(mode, 1920, 1080, 3840, 2160);
        CHECK_NEAR(scale.x, 1.0, 0.001);
        CHECK_NEAR(scale.y, 1.0, 0.001);
    }
}

TEST_CASE("fill overscans the axis that would otherwise leave a bar") {
    // 16:9 content on a 16:10 display is relatively wide, so filling means
    // growing horizontally and cropping the overflow.
    const FitScale scale = fitScale(FitMode::Fill, 1920, 1080, 1920, 1200);
    CHECK(scale.x > 1.0f);
    CHECK_NEAR(scale.y, 1.0, 0.001);
}

TEST_CASE("fit shrinks instead, leaving bars") {
    const FitScale scale = fitScale(FitMode::Fit, 1920, 1080, 1920, 1200);
    CHECK_NEAR(scale.x, 1.0, 0.001);
    CHECK(scale.y < 1.0f);
}

TEST_CASE("stretch always fills exactly") {
    const FitScale scale = fitScale(FitMode::Stretch, 1920, 1080, 1000, 2000);
    CHECK_NEAR(scale.x, 1.0, 0.001);
    CHECK_NEAR(scale.y, 1.0, 0.001);
}

TEST_CASE("a fit-mode effect is described only when there is one to describe") {
    CHECK(fitModeEffect(FitMode::Fill, 1920, 1080, 3840, 2160).empty());
    CHECK(!fitModeEffect(FitMode::Fill, 1920, 1080, 1920, 1200).empty());
    CHECK(!fitModeEffect(FitMode::Fit, 1920, 1080, 1920, 1200).empty());
}

TEST_CASE("fit-mode names round-trip through settings") {
    for (const FitMode mode : kAllFitModes) {
        CHECK(fitModeFromName(fitModeName(mode)) == mode);
    }
    CHECK(fitModeFromName("nonsense") == kDefaultFitMode);
}
