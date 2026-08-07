// Output sizing and the fit-mode transform.
#include "Testing.h"

#include "import/Transcoder.h"
#include "render/FitMode.h"

using namespace livewall;

namespace {

const TranscodePreset& ultraLight() { return Transcoder::presets()[0]; }
const TranscodePreset& balanced() { return Transcoder::presets()[1]; }
const TranscodePreset& native() { return Transcoder::presets()[2]; }

struct Size {
    int width = 0;
    int height = 0;
    bool operator==(const Size& other) const {
        return width == other.width && height == other.height;
    }
};

Size sizeFor(int sourceWidth, int sourceHeight, const TranscodePreset& preset,
             DisplayTarget display = {1920, 1080, 60}) {
    Size size;
    Transcoder::outputSize(sourceWidth, sourceHeight, preset, display, &size.width, &size.height);
    return size;
}

}  // namespace

TEST(output_size, fitsInsideTheLongestEdge) {
    const Size size = sizeFor(3840, 2160, balanced());
    EXPECT_EQ(size.width, 1920);
    EXPECT_EQ(size.height, 1080);
}

TEST(output_size, neverUpscalesPastTheSource) {
    // A 1280x720 source with a 1920 cap stays 1280x720. Inventing pixels costs
    // memory at playback and buys nothing.
    const Size size = sizeFor(1280, 720, balanced());
    EXPECT_EQ(size.width, 1280);
    EXPECT_EQ(size.height, 720);
}

TEST(output_size, portraitSourcesAreCappedOnTheirLongEdge) {
    const Size size = sizeFor(2160, 3840, balanced());
    EXPECT_EQ(size.height, 1920);
    EXPECT_EQ(size.width, 1080);
}

TEST(output_size, ultraLightIsSmaller) {
    const Size size = sizeFor(3840, 2160, ultraLight());
    EXPECT_EQ(size.width, 960);
    EXPECT_EQ(size.height, 540);
}

TEST(output_size, nativeCoversTheDisplay) {
    // A 16:9 source on a 16:10 panel: covering means the height decides, and the
    // result is wider than the panel.
    const Size size = sizeFor(3840, 2160, native(), DisplayTarget{1920, 1200, 60});
    EXPECT_TRUE(size.height >= 1200);
    EXPECT_TRUE(size.width >= 1920);
}

TEST(output_size, nativeStillNeverUpscales) {
    // A small source on a 4K panel. Covering would need 3x; 1:1 is the cap.
    const Size size = sizeFor(1280, 720, native(), DisplayTarget{3840, 2160, 60});
    EXPECT_EQ(size.width, 1280);
    EXPECT_EQ(size.height, 720);
}

TEST(output_size, dimensionsAreAlwaysEven) {
    // 4:2:0 chroma subsampling requires it, and an odd dimension is rejected by
    // the encoder with a message that does not mention parity.
    for (const int width : {1921, 1279, 999, 3}) {
        for (const int height : {1081, 721, 555, 3}) {
            const Size size = sizeFor(width, height, native(), DisplayTarget{4096, 4096, 60});
            EXPECT_EQ(size.width % 2, 0);
            EXPECT_EQ(size.height % 2, 0);
            EXPECT_TRUE(size.width >= 2);
            EXPECT_TRUE(size.height >= 2);
        }
    }
}

TEST(output_size, aSourceWithNoDimensionsStillProducesSomething) {
    const Size size = sizeFor(0, 0, balanced());
    EXPECT_TRUE(size.width > 0);
    EXPECT_TRUE(size.height > 0);
    EXPECT_EQ(size.width % 2, 0);
}

TEST(output_size, aspectRatioIsPreserved) {
    const Size size = sizeFor(3840, 1600, balanced());
    const double sourceAspect = 3840.0 / 1600.0;
    const double outputAspect = static_cast<double>(size.width) / size.height;
    // Within a pixel of rounding to even on each axis.
    EXPECT_NEAR(outputAspect, sourceAspect, 0.01);
}

// --- fit transform ----------------------------------------------------------

TEST(fit, matchingAspectRatiosAreIdentity) {
    for (const FitMode mode : {FitMode::Fill, FitMode::Fit, FitMode::Stretch}) {
        const FitTransform transform = fitTransform(mode, 1920, 1080, 3840, 2160);
        EXPECT_NEAR(transform.scaleX, 1.0, 1e-6);
        EXPECT_NEAR(transform.scaleY, 1.0, 1e-6);
        EXPECT_NEAR(transform.offsetX, 0.0, 1e-6);
        EXPECT_NEAR(transform.offsetY, 0.0, 1e-6);
    }
}

TEST(fit, stretchIsAlwaysIdentity) {
    const FitTransform transform = fitTransform(FitMode::Stretch, 1920, 1080, 1280, 1024);
    EXPECT_NEAR(transform.scaleX, 1.0, 1e-6);
    EXPECT_NEAR(transform.scaleY, 1.0, 1e-6);
}

TEST(fit, fillSamplesInsideTheTextureAndStaysCentred) {
    // 21:9 content on a 16:9 display: the sides are cropped, so the sampled
    // window is narrower than the texture and centred in it.
    const FitTransform transform = fitTransform(FitMode::Fill, 2560, 1080, 1920, 1080);
    EXPECT_TRUE(transform.scaleX < 1.0);
    EXPECT_NEAR(transform.scaleY, 1.0, 1e-6);
    EXPECT_NEAR(transform.offsetX * 2 + transform.scaleX, 1.0, 1e-5);
    // Every sampled coordinate stays in [0,1], which is what makes Fill have no
    // transparent region.
    EXPECT_TRUE(transform.offsetX >= 0.0);
    EXPECT_TRUE(transform.offsetX + transform.scaleX <= 1.0 + 1e-6);
}

TEST(fit, fillCropsTheOtherAxisForTallContent) {
    const FitTransform transform = fitTransform(FitMode::Fill, 1080, 1920, 1920, 1080);
    EXPECT_TRUE(transform.scaleY < 1.0);
    EXPECT_NEAR(transform.scaleX, 1.0, 1e-6);
}

TEST(fit, fitSamplesOutsideTheTextureToMakeBars) {
    // The letterbox is produced by sampling past the edge, which the fragment
    // shader turns transparent. A transform that stayed inside [0,1] would
    // produce no bars at all.
    const FitTransform transform = fitTransform(FitMode::Fit, 2560, 1080, 1920, 1080);
    EXPECT_TRUE(transform.scaleY > 1.0);
    EXPECT_TRUE(transform.offsetY < 0.0);
    EXPECT_NEAR(transform.offsetY * 2 + transform.scaleY, 1.0, 1e-5);
}

TEST(fit, degenerateSizesAreIdentityRatherThanNaN) {
    const FitTransform transform = fitTransform(FitMode::Fill, 0, 0, 1920, 1080);
    EXPECT_NEAR(transform.scaleX, 1.0, 1e-6);
    EXPECT_NEAR(transform.scaleY, 1.0, 1e-6);
}

TEST(fit, effectDescriptionIsEmptyWhenNothingHappens) {
    EXPECT_TRUE(fitModeEffect(FitMode::Fill, 1920, 1080, 3840, 2160).empty());
}

TEST(fit, effectDescriptionNamesTheCost) {
    const std::string fill = fitModeEffect(FitMode::Fill, 2560, 1080, 1920, 1080);
    EXPECT_TRUE(fill.find("Crops") != std::string::npos);
    EXPECT_TRUE(fill.find("width") != std::string::npos);

    const std::string fit = fitModeEffect(FitMode::Fit, 2560, 1080, 1920, 1080);
    EXPECT_TRUE(fit.find("Bars above and below") != std::string::npos);
}

TEST(fit, modeNamesRoundTrip) {
    for (const FitMode mode : {FitMode::Fill, FitMode::Fit, FitMode::Stretch}) {
        EXPECT_TRUE(fitModeFromString(fitModeToString(mode)) == mode);
    }
    // An unknown mode falls back rather than producing a fourth state.
    EXPECT_TRUE(fitModeFromString("cover") == FitMode::Fill);
    EXPECT_TRUE(fitModeFromString("STRETCH") == FitMode::Stretch);
}

// --- presets ----------------------------------------------------------------

TEST(presets, namesResolveAndAreStable) {
    EXPECT_EQ(Transcoder::presetNamed("Balanced").name, std::string("Balanced"));
    EXPECT_EQ(Transcoder::presetNamed("native").name, std::string("Native"));
    EXPECT_EQ(Transcoder::presetNamed("ultra").name, std::string("Ultra Light"));
    // The old macOS name for the top preset.
    EXPECT_EQ(Transcoder::presetNamed("Fidelity").name, std::string("Native"));
    // Anything unrecognised lands on the default rather than on nothing.
    EXPECT_EQ(Transcoder::presetNamed("nonsense").name, Transcoder::defaultPreset().name);
}

TEST(presets, frameRatesStayFrugal) {
    // The measured claim the presets are built on: frames cost, pixels do not.
    // A preset above 30 fps would quietly invalidate the README's numbers.
    for (const TranscodePreset& preset : Transcoder::presets()) {
        EXPECT_TRUE(preset.fps <= 24);
        EXPECT_TRUE(preset.fps >= 20);
    }
}
