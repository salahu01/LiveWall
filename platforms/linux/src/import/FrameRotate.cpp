#include "FrameRotate.h"

#include <cstring>

namespace livewall {
namespace {

// One sample position's worth of bytes, moved whole.
inline void copyElement(const std::uint8_t* src, std::uint8_t* dst, int elementBytes) {
    switch (elementBytes) {
        // The three sizes every format this port encodes to actually uses. Naming
        // them lets the compiler emit a load and a store instead of a call.
        case 1: *dst = *src; break;
        case 2: std::memcpy(dst, src, 2); break;
        case 4: std::memcpy(dst, src, 4); break;
        default: std::memcpy(dst, src, static_cast<std::size_t>(elementBytes)); break;
    }
}

}  // namespace

void rotatedSize(int width, int height, int degrees, int* outWidth, int* outHeight) {
    const int turn = ((degrees % 360) + 360) % 360;
    const bool swaps = turn == 90 || turn == 270;
    if (outWidth != nullptr) *outWidth = swaps ? height : width;
    if (outHeight != nullptr) *outHeight = swaps ? width : height;
}

void rotatePlane(const std::uint8_t* src, int srcStride, const PlaneGeometry& geometry,
                 std::uint8_t* dst, int dstStride, int degrees) {
    if (src == nullptr || dst == nullptr) return;
    if (geometry.width <= 0 || geometry.height <= 0 || geometry.elementBytes <= 0) return;

    const int width = geometry.width;
    const int height = geometry.height;
    const int bytes = geometry.elementBytes;
    const int turn = ((degrees % 360) + 360) % 360;

    switch (turn) {
        case 90:
            // Clockwise: the source's bottom-left corner becomes the output's
            // top-left, so dst(x, y) = src(y, height - 1 - x). The output is
            // height x width.
            for (int y = 0; y < width; ++y) {
                std::uint8_t* dstRow = dst + static_cast<std::size_t>(y) * dstStride;
                for (int x = 0; x < height; ++x) {
                    const std::uint8_t* srcElement =
                        src + static_cast<std::size_t>(height - 1 - x) * srcStride +
                        static_cast<std::size_t>(y) * bytes;
                    copyElement(srcElement, dstRow + static_cast<std::size_t>(x) * bytes, bytes);
                }
            }
            break;

        case 180:
            // dst(x, y) = src(width - 1 - x, height - 1 - y). Same geometry.
            for (int y = 0; y < height; ++y) {
                std::uint8_t* dstRow = dst + static_cast<std::size_t>(y) * dstStride;
                const std::uint8_t* srcRow =
                    src + static_cast<std::size_t>(height - 1 - y) * srcStride;
                for (int x = 0; x < width; ++x) {
                    copyElement(srcRow + static_cast<std::size_t>(width - 1 - x) * bytes,
                                dstRow + static_cast<std::size_t>(x) * bytes, bytes);
                }
            }
            break;

        case 270:
            // Counter-clockwise: the source's top-right becomes the output's
            // top-left, so dst(x, y) = src(width - 1 - y, x).
            for (int y = 0; y < width; ++y) {
                std::uint8_t* dstRow = dst + static_cast<std::size_t>(y) * dstStride;
                for (int x = 0; x < height; ++x) {
                    const std::uint8_t* srcElement =
                        src + static_cast<std::size_t>(x) * srcStride +
                        static_cast<std::size_t>(width - 1 - y) * bytes;
                    copyElement(srcElement, dstRow + static_cast<std::size_t>(x) * bytes, bytes);
                }
            }
            break;

        default:
            // 0, and anything that is not a whole quarter turn. A straight copy
            // rather than a guess.
            for (int y = 0; y < height; ++y) {
                std::memcpy(dst + static_cast<std::size_t>(y) * dstStride,
                            src + static_cast<std::size_t>(y) * srcStride,
                            static_cast<std::size_t>(width) * bytes);
            }
            break;
    }
}

}  // namespace livewall
