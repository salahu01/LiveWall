// Per-import frame rate and rotation, and the plane rotation kernel.
//
// The kernel is the part worth testing hardest: it is the only place in this
// port that moves pixels by hand, a transposition bug shows up as a sheared or
// mirrored wallpaper rather than a crash, and nothing downstream would notice.
// Planes here are built by hand with deliberately over-wide strides, since a
// real sample's stride almost never equals its width.
//
// Deliberately free of <windows.h>: this file includes only FrameRotate.h and
// ImportOptions.h, so the arithmetic can be compiled and run off-platform while
// the rest of the port has no machine that can build it.

#include "TestHarness.h"

#include <cstdint>
#include <vector>

#include "import/FrameRotate.h"
#include "import/ImportOptions.h"

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

bool sameElement(const Plane& src, int sx, int sy, const Plane& dst, int dx, int dy) {
    const std::uint8_t* a = src.at(sx, sy);
    const std::uint8_t* b = dst.at(dx, dy);
    for (int byte = 0; byte < src.elementBytes; ++byte) {
        if (a[byte] != b[byte]) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("no opinion falls back to the preset's rate") {
    CHECK_EQ(ImportOptions{}.preferredFps(24), 24);
    CHECK_EQ(ImportOptions{}.preferredFps(20), 20);
}

TEST_CASE("a chosen rate wins over the preset") {
    ImportOptions options;
    options.fps = 30;
    CHECK_EQ(options.preferredFps(24), 30);
    // Below the preset too — the point is the user's number, not a raised
    // ceiling.
    options.fps = 15;
    CHECK_EQ(options.preferredFps(24), 15);
}

TEST_CASE("a rate outside the offered range is clamped rather than rejected") {
    ImportOptions options;
    options.fps = 9000;
    CHECK_EQ(options.preferredFps(24), ImportOptions::kMaximumFps);
    options.fps = 1;
    CHECK_EQ(options.preferredFps(24), ImportOptions::kMinimumFps);
}

// pacedFPS gives up below 12 and hands the request back unsnapped, so offering
// a rate under that would return the judder the pipeline exists to avoid.
TEST_CASE("the offered floor is at or above the pacer's snapping floor") {
    CHECK(ImportOptions::kMinimumFps >= 12);
    for (int i = 0; i < ImportOptions::kOfferedFpsCount; ++i) {
        CHECK(ImportOptions::kOfferedFps[i] >= ImportOptions::kMinimumFps);
        CHECK(ImportOptions::kOfferedFps[i] <= ImportOptions::kMaximumFps);
    }
}

TEST_CASE("angles normalise into one revolution") {
    CHECK_EQ(ImportOptions::normalised(-90), 270);
    CHECK_EQ(ImportOptions::normalised(450), 90);
    CHECK_EQ(ImportOptions::normalised(360), 0);
    CHECK_EQ(ImportOptions::normalised(-360), 0);
}

TEST_CASE("only quarter turns are accepted") {
    ImportOptions options;
    for (const int degrees : {0, 90, 180, 270, 360, -90}) {
        options.rotationDegrees = degrees;
        CHECK(options.isQuarterTurn());
    }
    for (const int degrees : {1, 45, 89, 100}) {
        options.rotationDegrees = degrees;
        CHECK(!options.isQuarterTurn());
    }
}

TEST_CASE("quarter turns swap the edges and half turns do not") {
    ImportOptions options;
    options.rotationDegrees = 0;
    CHECK(!options.swapsEdges());
    options.rotationDegrees = 90;
    CHECK(options.swapsEdges());
    options.rotationDegrees = 180;
    CHECK(!options.swapsEdges());
    options.rotationDegrees = 270;
    CHECK(options.swapsEdges());
}

TEST_CASE("every offered rotation is a quarter turn inside one revolution") {
    for (int i = 0; i < ImportOptions::kRotationCount; ++i) {
        const int degrees = ImportOptions::kRotations[i];
        CHECK_EQ(degrees % 90, 0);
        CHECK_EQ(ImportOptions::normalised(degrees), degrees);
    }
}

TEST_CASE("rotated size swaps only on quarter turns") {
    int width = 0;
    int height = 0;

    rotatedSize(1920, 1080, 0, &width, &height);
    CHECK_EQ(width, 1920);
    CHECK_EQ(height, 1080);

    rotatedSize(1920, 1080, 90, &width, &height);
    CHECK_EQ(width, 1080);
    CHECK_EQ(height, 1920);

    rotatedSize(1920, 1080, 180, &width, &height);
    CHECK_EQ(width, 1920);
    CHECK_EQ(height, 1080);

    rotatedSize(1920, 1080, 270, &width, &height);
    CHECK_EQ(width, 1080);
    CHECK_EQ(height, 1920);
}

// Clockwise: the source's bottom-left corner becomes the output's top-left.
TEST_CASE("ninety degrees maps bottom-left to top-left") {
    Plane source(5, 3, 1, 7);
    paint(source);
    Plane destination(3, 5, 1, 4);

    rotatePlane(source.bytes.data(), source.stride, source.geometry(), destination.bytes.data(),
                destination.stride, 90);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            CHECK(sameElement(source, x, y, destination, source.height - 1 - y, x));
        }
    }
}

// Counter-clockwise: the source's top-right becomes the output's top-left.
TEST_CASE("two hundred and seventy degrees maps top-right to top-left") {
    Plane source(5, 3, 1, 3);
    paint(source);
    Plane destination(3, 5, 1, 9);

    rotatePlane(source.bytes.data(), source.stride, source.geometry(), destination.bytes.data(),
                destination.stride, 270);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            CHECK(sameElement(source, x, y, destination, y, source.width - 1 - x));
        }
    }
}

TEST_CASE("one hundred and eighty degrees reverses both axes") {
    Plane source(6, 4, 1, 5);
    paint(source);
    Plane destination(6, 4, 1, 2);

    rotatePlane(source.bytes.data(), source.stride, source.geometry(), destination.bytes.data(),
                destination.stride, 180);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            CHECK(sameElement(source, x, y, destination, source.width - 1 - x,
                              source.height - 1 - y));
        }
    }
}

TEST_CASE("zero degrees is a faithful copy") {
    Plane source(6, 4, 1, 5);
    paint(source);
    Plane destination(6, 4, 1, 11);

    rotatePlane(source.bytes.data(), source.stride, source.geometry(), destination.bytes.data(),
                destination.stride, 0);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            CHECK(sameElement(source, x, y, destination, x, y));
        }
    }
}

// An angle that is not a quarter turn is copied through rather than guessed at.
TEST_CASE("an arbitrary angle is treated as upright") {
    Plane source(4, 3, 1, 2);
    paint(source);
    Plane destination(4, 3, 1, 6);

    rotatePlane(source.bytes.data(), source.stride, source.geometry(), destination.bytes.data(),
                destination.stride, 45);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            CHECK(sameElement(source, x, y, destination, x, y));
        }
    }
}

// NV12's chroma plane stores U and V at one position, and P010's stores four
// bytes there. Both have to travel as a unit or the colours separate from the
// luma they belong to.
TEST_CASE("multi-byte elements travel whole") {
    for (const int elementBytes : {2, 4}) {
        Plane source(5, 3, elementBytes, 6);
        paint(source);
        Plane destination(3, 5, elementBytes, 3);

        rotatePlane(source.bytes.data(), source.stride, source.geometry(),
                    destination.bytes.data(), destination.stride, 90);

        for (int y = 0; y < source.height; ++y) {
            for (int x = 0; x < source.width; ++x) {
                CHECK(sameElement(source, x, y, destination, source.height - 1 - y, x));
            }
        }
    }
}

// Four quarter turns is the identity — the cheapest check that the mapping is a
// rotation at all and not a shear that happens to look right on a square.
TEST_CASE("four quarter turns return the original") {
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
            CHECK(sameElement(original, x, y, d, x, y));
        }
    }
}

// 90 then 270 must also come back, which catches a sign error that four
// identical turns would hide.
TEST_CASE("a turn and its opposite cancel") {
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
            CHECK(sameElement(original, x, y, back, x, y));
        }
    }
}

// The shape rotateSample relies on: an NV12 frame is a full-resolution luma
// plane and a half-resolution chroma plane whose elements are twice as wide, so
// both planes land on the same stride. If that ever stopped holding, the
// chroma offset arithmetic in rotateSample would be wrong.
TEST_CASE("NV12 and P010 planes share a stride") {
    for (const int lumaBytes : {1, 2}) {
        const int width = 64;
        const int lumaStride = width * lumaBytes;
        const int chromaStride = (width / 2) * (lumaBytes * 2);
        CHECK_EQ(chromaStride, lumaStride);
    }
}
