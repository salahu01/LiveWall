// The procedural wallpaper: one triangle, one fragment shader, no decoder.
//
// The default when nothing is selected, and the fallback whenever a video
// cannot be prepared. It has no file, no codec and no VA-API device, so it is
// the one mode that works on every machine this port will ever run on —
// including a container with llvmpipe, which is what CI has.
//
// Its cost is a wakeup per tick and a full-screen triangle. See gradient.frag
// for why that is not zero here the way it is on macOS.
#pragma once

#include <string>

#include "render/GlProgram.h"
#include "render/WallpaperSource.h"

namespace livewall {

class GradientSource final : public WallpaperSource {
public:
    explicit GradientSource(int fps) : fps_(fps > 0 ? fps : 10) {}

    bool prepare(EglDevice& egl) override;
    void activate() override { active_ = true; }
    void deactivate() override { active_ = false; }
    bool isActive() const override { return active_; }

    int framesPerSecond() const override { return fps_; }
    bool render(Surface& surface, FitMode mode) override;
    std::string summary() const override;

private:
    GlProgram program_;
    int fps_ = 10;
    bool active_ = false;
    bool ready_ = false;
    // Monotonic milliseconds at prepare(), so the animation phase does not jump
    // when the source is torn down and rebuilt — which happens on every
    // occlusion cycle.
    long long originMs_ = 0;
};

}  // namespace livewall
