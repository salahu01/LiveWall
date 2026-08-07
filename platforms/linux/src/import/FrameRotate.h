// Quarter-turn rotation of a decoded frame's planes.
//
// swscale converts and scales but cannot rotate, and the obvious alternative —
// libavfilter's `transpose` — is the wrong trade here. `FFmpeg.h` binds its four
// libraries as a *matched set* and fails whole if any symbol is missing, so
// making libavfilter a fifth member would turn a machine that merely lacks it
// into a machine with no video import at all. Rotating the frame directly costs
// one pass over the pixels, once, at import time, and adds nothing to the link
// line or the dlopen set.
//
// The work is a pure memory transpose, so it lives apart from the transcoder and
// is tested without FFmpeg, a decoder or a display.
#pragma once

#include <cstdint>

namespace livewall {

// The plane geometry a rotation needs, in elements rather than bytes.
//
// One "element" is everything stored at a single sample position: a byte of Y in
// yuv420p, two bytes of Y in yuv420p10le, an interleaved U+V pair in nv12, and
// four bytes of interleaved U+V in p010le. Rotating moves sample positions, and
// everything at a position travels together — which is exactly what makes the
// interleaved chroma formats work without a special case.
struct PlaneGeometry {
    int width = 0;         // in elements
    int height = 0;        // in elements
    int elementBytes = 1;  // bytes per sample position
};

// Output dimensions of `width` x `height` after `degrees` of rotation.
void rotatedSize(int width, int height, int degrees, int* outWidth, int* outHeight);

// Rotates one plane clockwise by `degrees` (0, 90, 180 or 270).
//
// `dst` must have room for the rotated geometry — for a quarter turn that is
// `height` x `width`, not `width` x `height`. Strides are in bytes and may
// exceed the row's used width, which is ordinary for both FFmpeg frames and
// hardware buffers.
//
// A `degrees` that is not a whole quarter turn is treated as 0 rather than
// guessed at: an arbitrary angle needs a resampling filter and a decision about
// cropping, and silently rotating by the nearest quarter would be a worse answer
// than not rotating.
void rotatePlane(const std::uint8_t* src, int srcStride, const PlaneGeometry& geometry,
                 std::uint8_t* dst, int dstStride, int degrees);

}  // namespace livewall
