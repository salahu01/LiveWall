// Owns one output's surface and the source drawing into it, and decides moment
// to moment whether that source should be running at all.
//
// Rendering is allowed only when all of these hold:
//   - enough of this output's desktop is uncovered (X11 only — see below),
//   - the display server wants a frame (Wayland's frame callback; always true
//     on X11),
//   - no machine-wide condition blocks it (lock, sleep, power-saver...).
//
// Deactivation is delayed slightly so that transient occlusion during a window
// animation or a workspace switch does not tear down and rebuild the decoder
// several times a second. Activation is immediate — there is no reason to make
// the user wait to see their wallpaper.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "platform/Backend.h"
#include "render/WallpaperSource.h"
#include "support/PowerMonitor.h"

namespace livewall {

class EglDevice;

class OutputController {
public:
    OutputController(OutputInfo info, Backend& backend, EglDevice& egl, PowerMonitor& power);
    ~OutputController();

    OutputController(const OutputController&) = delete;
    OutputController& operator=(const OutputController&) = delete;

    // Creates the surface. Separate from the constructor because it can fail
    // and the engine needs to drop the controller rather than keep a broken
    // one.
    bool createSurface();

    void setSource(std::unique_ptr<WallpaperSource> source);

    // Makes this output's surface current so a source can compile its shaders.
    //
    // Needed because GL programs are context state and there is no context
    // until some surface is current — a source prepared before that silently
    // gets 0 from glCreateShader and reports a compile failure with an empty
    // log, which is a confusing way to find out.
    bool makeContextCurrent();
    void updateInfo(const OutputInfo& info);

    // Re-runs the gates. Cheap unless the visibility poll is due.
    void evaluate(std::int64_t nowMs);

    // Renders and presents if a frame is due. Returns true if it presented.
    bool tick(std::int64_t nowMs, FitMode mode);

    // When this output next needs attention, in absolute monotonic
    // milliseconds. The event loop sleeps until the earliest across all
    // outputs.
    std::int64_t nextDeadlineMs() const;

    bool isRendering() const;
    bool isNearlyCovered() const { return nearlyCovered_; }
    const OutputInfo& info() const { return info_; }
    std::string sourceSummary() const;

    // Set once at construction from LIVEWALL_FORCE_RENDER. Exists so playback
    // cost can be measured on a machine whose desktop is behind a terminal —
    // the occlusion gate otherwise tears the decoder down and every reading
    // comes back as the idle baseline. Never set in normal use.
    static bool forceRender();

private:
    bool updateCoverageLatch();

    // Stop once this little of the desktop is left uncovered, resume once this
    // much is back. The gap is deliberate: a single threshold makes a window
    // edge resting near it toggle the decoder repeatedly.
    static constexpr double kStopBelowFraction = 0.08;
    static constexpr double kResumeAboveFraction = 0.15;

    static constexpr std::int64_t kDeactivateDelayMs = 400;

    // Windows can be moved and resized without any notification a client can
    // observe, so the uncovered fraction has to be sampled. The macOS port
    // profiled this poll at 11 main-thread samples against 7 for the entire
    // frame pump over the same six seconds — the gate was costing more than the
    // video it was gating. Walking the window list is not free, so it happens
    // rarely: a wallpaper that takes four seconds to notice it has been covered
    // has cost nothing anyone can perceive.
    static constexpr std::int64_t kVisibilityPollMs = 4000;

    OutputInfo info_;
    Backend& backend_;
    EglDevice& egl_;
    PowerMonitor& power_;

    std::unique_ptr<Surface> surface_;
    std::unique_ptr<WallpaperSource> source_;

    // Latched through the hysteresis band rather than recomputed as a plain
    // comparison each time.
    bool nearlyCovered_ = false;

    std::int64_t nextVisibilityPollMs_ = 0;
    std::int64_t nextFrameMs_ = 0;
    // Zero when no teardown is pending.
    std::int64_t deactivateAtMs_ = 0;
};

}  // namespace livewall
