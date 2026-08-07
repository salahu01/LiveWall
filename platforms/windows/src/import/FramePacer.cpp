#include "import/FramePacer.h"

#include <algorithm>

namespace livewall {

FramePacer::FramePacer(int fps) {
    const int rate = std::max(1, fps);
    frameDurationHns_ = kHnsPerSecond / rate;
}

FramePacer::Decision FramePacer::accept(long long sourceTimeHns) {
    if (sourceTimeHns < 0) {
        ++dropped_;
        return {};
    }

    if (sourceTimeHns < nextSourceTimeHns_) {
        ++dropped_;
        return {};
    }

    Decision decision;
    decision.keep = true;
    decision.outputTimeHns = outputTimeHns_;
    decision.durationHns = frameDurationHns_;

    ++kept_;
    outputTimeHns_ += frameDurationHns_;

    // Advance the source cursor past `sourceTimeHns` so a source slower than
    // the target grid cannot make us emit the same instant twice.
    do {
        nextSourceTimeHns_ += frameDurationHns_;
    } while (nextSourceTimeHns_ <= sourceTimeHns);

    return decision;
}

}  // namespace livewall
