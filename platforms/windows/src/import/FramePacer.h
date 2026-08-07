// Resamples a stream of video frames onto an exact 1/fps grid.
//
// Frames arriving between grid points are dropped; the ones that survive are
// stamped with clean, evenly spaced presentation times starting at zero. That
// gives the encoder a genuinely constant frame rate rather than the source
// cadence with a frame-rate hint attached.
//
// This matters beyond tidiness: the playback path pulls exactly one frame per
// tick at the rate recorded in the library, so a file whose real cadence
// differs from its recorded rate plays at the wrong speed.
//
// The logic is identical to the macOS `FramePacer` and is deliberately kept
// free of Media Foundation types so both platforms' tests exercise the same
// arithmetic.
//
// Not thread-safe: it is driven from the transcoder's single pump.
#pragma once

namespace livewall {

class FramePacer {
public:
    // Times are in Media Foundation's 100 ns units.
    static constexpr long long kHnsPerSecond = 10'000'000LL;

    explicit FramePacer(int fps);

    struct Decision {
        bool keep = false;
        // Presentation time to stamp on the frame, valid only when `keep`.
        long long outputTimeHns = 0;
        long long durationHns = 0;
    };

    // Decides what to do with a frame that arrived at `sourceTimeHns`.
    Decision accept(long long sourceTimeHns);

    int kept() const { return kept_; }
    int dropped() const { return dropped_; }
    long long frameDurationHns() const { return frameDurationHns_; }

private:
    long long frameDurationHns_ = 0;
    // Next position on the source timeline we still want a frame for.
    long long nextSourceTimeHns_ = 0;
    // Presentation time to stamp on the next frame we keep.
    long long outputTimeHns_ = 0;

    int kept_ = 0;
    int dropped_ = 0;
};

}  // namespace livewall
