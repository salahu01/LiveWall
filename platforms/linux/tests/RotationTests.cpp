// The per-import frame rate and rotation rules, and the plane rotation kernel.
//
// The kernel is the part worth testing hardest: it is the only place in this
// port that moves pixels by hand, a transposition bug shows up as a sheared or
// mirrored wallpaper rather than a crash, and nothing downstream would notice.
// Planes here are built by hand with deliberately over-wide strides, since a
// real AVFrame's linesize almost never equals its width.
#include "Testing.h"

#include <cstdint>
#include <vector>

#include "import/FrameRotate.h"
#include "import/ImportOptions.h"
#include "import/Transcoder.h"

using namespace livewall;

namespace {

// A plane whose element at (x, y) is a distinct value, so a rotation that
// transposes, mirrors or shears is visible in the result rather than plausible.
struct Plane {
    std::vector<std::uint8_t> bytes;
    int stride = 0;
    int width = 0;
    int height = 0;
    int elementBytes = 1;

    Plane(int w, int h, int elements, int extraStride)
        : stride(w * elements + extraStride), width(w), height(h), elementBytes(elements) {
        bytes.assign(static_cast<std::size_t>(stride) * h, 0xEE);
    }

    std::uint8_t* at(int x, int y) {
        return bytes.data() + static_cast<std::size_t>(y) * stride +
               static_cast<std::size_t>(x) * elementBytes;
    }

    const std::uint8_t* at(int x, int y) const {
        return bytes.data() + static_cast<std::size_t>(y) * stride +
               static_cast<std::size_t>(x) * elementBytes;
    }

    PlaneGeometry geometry() const { return PlaneGeometry{width, height, elementBytes}; }
};

// Fills every element with a value derived from its position.
void paint(Plane& plane) {
    for (int y = 0; y < plane.height; ++y) {
        for (int x = 0; x < plane.width; ++x) {
            std::uint8_t* element = plane.at(x, y);
            for (int byte = 0; byte < plane.elementBytes; ++byte) {
                element[byte] = static_cast<std::uint8_t>(y * plane.width + x + byte * 100 + 1);
            }
        }
    }
}

// True when dst(dx, dy) holds exactly the element src(sx, sy) does.
bool sameElement(const Plane& src, int sx, int sy, const Plane& dst, int dx, int dy) {
    const std::uint8_t* a = src.at(sx, sy);
    const std::uint8_t* b = dst.at(dx, dy);
    for (int byte = 0; byte < src.elementBytes; ++byte) {
        if (a[byte] != b[byte]) return false;
    }
    return true;
}

}  // namespace

// MARK: - Frame rate

TEST(import_options, noOpinionFallsBackToThePresetRate) {
    EXPECT_EQ(ImportOptions{}.preferredFps(24), 24);
    EXPECT_EQ(ImportOptions{}.preferredFps(20), 20);
}

TEST(import_options, aChosenRateWinsOverThePreset) {
    ImportOptions options;
    options.fps = 30;
    EXPECT_EQ(options.preferredFps(24), 30);
    // Below the preset too — the point is the user's number, not a raised
    // ceiling.
    options.fps = 15;
    EXPECT_EQ(options.preferredFps(24), 15);
}

// A typo on the command line must not reach the encoder's bitrate maths.
TEST(import_options, aRateOutsideTheOfferedRangeIsClamped) {
    ImportOptions options;
    options.fps = 9000;
    EXPECT_EQ(options.preferredFps(24), ImportOptions::kMaximumFps);
    options.fps = 1;
    EXPECT_EQ(options.preferredFps(24), ImportOptions::kMinimumFps);
}

// pacedFps gives up below 12 and hands the request back unsnapped, so offering
// a rate under that would return the judder the pipeline exists to avoid.
TEST(import_options, theFloorIsAtOrAboveThePacerSnappingFloor) {
    EXPECT_TRUE(ImportOptions::kMinimumFps >= 12);
}

// The contract the CLI promises: your number is the input to the pacing rule,
// not an exemption from it.
TEST(import_options, aChosenRateStillGoesThroughTheDisplaySnap) {
    ImportOptions options;
    options.fps = 30;
    const int chosen = options.preferredFps(24);
    EXPECT_EQ(Transcoder::pacedFps(chosen, 90), 30);
    EXPECT_EQ(Transcoder::pacedFps(chosen, 100), 25);
}

// MARK: - Rotation arithmetic

TEST(import_options, anglesNormaliseIntoOneRevolution) {
    EXPECT_EQ(ImportOptions::normalised(-90), 270);
    EXPECT_EQ(ImportOptions::normalised(450), 90);
    EXPECT_EQ(ImportOptions::normalised(360), 0);
    EXPECT_EQ(ImportOptions::normalised(-360), 0);
}

TEST(import_options, onlyQuarterTurnsAreAccepted) {
    ImportOptions options;
    for (const int degrees : {0, 90, 180, 270, 360, -90}) {
        options.rotationDegrees = degrees;
        EXPECT_TRUE(options.isQuarterTurn());
    }
    for (const int degrees : {1, 45, 89, 100}) {
        options.rotationDegrees = degrees;
        EXPECT_FALSE(options.isQuarterTurn());
    }
}

TEST(import_options, quarterTurnsSwapTheEdgesAndHalfTurnsDoNot) {
    ImportOptions options;
    options.rotationDegrees = 0;
    EXPECT_FALSE(options.swapsEdges());
    options.rotationDegrees = 90;
    EXPECT_TRUE(options.swapsEdges());
    options.rotationDegrees = 180;
    EXPECT_FALSE(options.swapsEdges());
    options.rotationDegrees = 270;
    EXPECT_TRUE(options.swapsEdges());
}

TEST(rotation, rotatedSizeSwapsOnlyOnQuarterTurns) {
    int width = 0;
    int height = 0;

    rotatedSize(1920, 1080, 0, &width, &height);
    EXPECT_EQ(width, 1920);
    EXPECT_EQ(height, 1080);

    rotatedSize(1920, 1080, 90, &width, &height);
    EXPECT_EQ(width, 1080);
    EXPECT_EQ(height, 1920);

    rotatedSize(1920, 1080, 180, &width, &height);
    EXPECT_EQ(width, 1920);
    EXPECT_EQ(height, 1080);

    rotatedSize(1920, 1080, 270, &width, &height);
    EXPECT_EQ(width, 1080);
    EXPECT_EQ(height, 1920);
}

// MARK: - The kernel

// Clockwise: the source's bottom-left corner becomes the output's top-left.
TEST(rotation, ninetyDegreesMapsBottomLeftToTopLeft) {
    Plane source(5, 3, 1, 7);
    paint(source);
    Plane destination(3, 5, 1, 4);

    rotatePlane(source.bytes.data(), source.stride, source.geometry(),
                destination.bytes.data(), destination.stride, 90);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            // dst(x', y') where x' = height - 1 - y, y' = x.
            EXPECT_TRUE(sameElement(source, x, y, destination, source.height - 1 - y, x));
        }
    }
}

// Counter-clockwise: the source's top-right becomes the output's top-left.
TEST(rotation, twoSeventyMapsTopRightToTopLeft) {
    Plane source(5, 3, 1, 3);
    paint(source);
    Plane destination(3, 5, 1, 9);

    rotatePlane(source.bytes.data(), source.stride, source.geometry(),
                destination.bytes.data(), destination.stride, 270);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            EXPECT_TRUE(sameElement(source, x, y, destination, y, source.width - 1 - x));
        }
    }
}

TEST(rotation, oneEightyReversesBothAxes) {
    Plane source(6, 4, 1, 5);
    paint(source);
    Plane destination(6, 4, 1, 2);

    rotatePlane(source.bytes.data(), source.stride, source.geometry(),
                destination.bytes.data(), destination.stride, 180);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            EXPECT_TRUE(sameElement(source, x, y, destination, source.width - 1 - x,
                                    source.height - 1 - y));
        }
    }
}

TEST(rotation, zeroDegreesIsAFaithfulCopy) {
    Plane source(6, 4, 1, 5);
    paint(source);
    Plane destination(6, 4, 1, 11);

    rotatePlane(source.bytes.data(), source.stride, source.geometry(),
                destination.bytes.data(), destination.stride, 0);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            EXPECT_TRUE(sameElement(source, x, y, destination, x, y));
        }
    }
}

// An angle that is not a quarter turn is copied through rather than guessed at.
TEST(rotation, anArbitraryAngleIsTreatedAsUpright) {
    Plane source(4, 3, 1, 2);
    paint(source);
    Plane destination(4, 3, 1, 6);

    rotatePlane(source.bytes.data(), source.stride, source.geometry(),
                destination.bytes.data(), destination.stride, 45);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            EXPECT_TRUE(sameElement(source, x, y, destination, x, y));
        }
    }
}

// nv12's chroma plane stores U and V at one position, and p010le's stores four
// bytes there. Both have to travel as a unit or the colours separate from the
// luma they belong to.
TEST(rotation, multiByteElementsTravelWhole) {
    for (const int elementBytes : {2, 4}) {
        Plane source(5, 3, elementBytes, 6);
        paint(source);
        Plane destination(3, 5, elementBytes, 3);

        rotatePlane(source.bytes.data(), source.stride, source.geometry(),
                    destination.bytes.data(), destination.stride, 90);

        for (int y = 0; y < source.height; ++y) {
            for (int x = 0; x < source.width; ++x) {
                EXPECT_TRUE(sameElement(source, x, y, destination, source.height - 1 - y, x));
            }
        }
    }
}

// Four quarter turns is the identity — the cheapest check that the mapping is a
// rotation at all and not a shear that happens to look right on a square.
TEST(rotation, fourQuarterTurnsReturnTheOriginal) {
    Plane original(5, 3, 2, 4);
    paint(original);

    Plane a(3, 5, 2, 1);
    Plane b(5, 3, 2, 7);
    Plane c(3, 5, 2, 2);
    Plane d(5, 3, 2, 5);

    rotatePlane(original.bytes.data(), original.stride, original.geometry(), a.bytes.data(),
                a.stride, 90);
    rotatePlane(a.bytes.data(), a.stride, a.geometry(), b.bytes.data(), b.stride, 90);
    rotatePlane(b.bytes.data(), b.stride, b.geometry(), c.bytes.data(), c.stride, 90);
    rotatePlane(c.bytes.data(), c.stride, c.geometry(), d.bytes.data(), d.stride, 90);

    for (int y = 0; y < original.height; ++y) {
        for (int x = 0; x < original.width; ++x) {
            EXPECT_TRUE(sameElement(original, x, y, d, x, y));
        }
    }
}

// 90 then 270 must also come back, which catches a sign error that four
// identical turns would hide.
TEST(rotation, aTurnAndItsOppositeCancel) {
    Plane original(6, 4, 1, 3);
    paint(original);

    Plane turned(4, 6, 1, 5);
    Plane back(6, 4, 1, 2);

    rotatePlane(original.bytes.data(), original.stride, original.geometry(), turned.bytes.data(),
                turned.stride, 90);
    rotatePlane(turned.bytes.data(), turned.stride, turned.geometry(), back.bytes.data(),
                back.stride, 270);

    for (int y = 0; y < original.height; ++y) {
        for (int x = 0; x < original.width; ++x) {
            EXPECT_TRUE(sameElement(original, x, y, back, x, y));
        }
    }
}

// The bug this guards: a 1920x1080 source turned a quarter is a portrait clip.
// Sizing it as landscape would fit 1920 to maxEdge and leave the real long edge
// fitted to nothing.
TEST(rotation, aTurnedClipIsSizedAgainstTheEdgeItActuallyHas) {
    const TranscodePreset& preset = Transcoder::presetNamed("ultra");
    DisplayTarget display;

    int width = 0;
    int height = 0;
    // The transcoder swaps the source edges before sizing when the turn does.
    Transcoder::outputSize(1080, 1920, preset, display, &width, &height);

    EXPECT_TRUE(height > width);
    EXPECT_EQ(width > height ? width : height, preset.maxEdge);
}
