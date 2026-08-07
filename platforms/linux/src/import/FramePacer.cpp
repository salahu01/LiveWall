#include "import/FramePacer.h"

#include <algorithm>

namespace livewall {

FramePacer::FramePacer(int fps) : fps_(std::max(1, fps)) {}

std::int64_t FramePacer::gridPointMicroseconds(std::int64_t step) const {
    return step * 1000000 / fps_;
}

bool FramePacer::accept(std::int64_t sourceMicroseconds, std::int64_t* outputFrameIndex) {
    if (sourceMicroseconds < gridPointMicroseconds(nextGridStep_)) {
        ++dropped_;
        return false;
    }

    *outputFrameIndex = kept_;
    ++kept_;

    // Advance past this frame's own timestamp, so a source slower than the
    // target grid cannot make the same instant be emitted twice — which would
    // otherwise happen with, say, a 12 fps source paced to 24.
    do {
        ++nextGridStep_;
    } while (gridPointMicroseconds(nextGridStep_) <= sourceMicroseconds);

    return true;
}

}  // namespace livewall
