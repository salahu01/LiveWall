// Resamples a stream of decoded frames onto an exact 1/fps grid.
//
// Frames arriving between grid points are dropped; the ones that survive get
// clean, evenly spaced presentation times starting at zero. That gives the
// encoder a genuinely constant frame rate rather than the source's cadence with
// a frame-rate hint attached.
//
// This matters beyond tidiness. The playback path pulls exactly one frame per
// tick at the rate recorded in the library, so a file whose real cadence
// differs from its recorded rate plays at the wrong speed — a 30 fps source
// tagged as 24 runs 20% slow, forever, and looks like a bug in the decoder.
//
// Not thread-safe: it is driven from the transcoder's single loop.
#pragma once

#include <cstdint>

namespace livewall {

class FramePacer {
public:
    explicit FramePacer(int fps);

    // `sourceMicroseconds` is the frame's presentation time on the source
    // timeline. Returns true when the frame lands on the grid, writing its
    // output frame number — which is a presentation timestamp in a 1/fps time
    // base, so the caller does no arithmetic.
    bool accept(std::int64_t sourceMicroseconds, std::int64_t* outputFrameIndex);

    int kept() const { return kept_; }
    int dropped() const { return dropped_; }
    int fps() const { return fps_; }

private:
    // The next grid point we still want a frame for, in microseconds.
    // Recomputed from an integer step count rather than accumulated, because
    // 1/24 s does not divide a microsecond evenly and adding a rounded step a
    // few thousand times drifts by whole frames over a long clip.
    std::int64_t gridPointMicroseconds(std::int64_t step) const;

    int fps_ = 24;
    std::int64_t nextGridStep_ = 0;
    int kept_ = 0;
    int dropped_ = 0;
};

}  // namespace livewall
