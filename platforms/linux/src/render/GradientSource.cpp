#include "render/GradientSource.h"

#include <ctime>

#include "shaders/gradient_frag.h"
#include "shaders/wallpaper_vert.h"

#include "render/EglDevice.h"
#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {
namespace {

long long monotonicMs() {
    timespec now = {};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

}  // namespace

bool GradientSource::prepare(EglDevice& egl) {
    (void)egl;
    // A context is already current on some surface by the time the engine calls
    // this — programs are context state, not surface state, so one compile
    // serves every output.
    ready_ = program_.build("gradient", kwallpaper_vert, kgradient_frag);
    originMs_ = monotonicMs();
    return ready_;
}

bool GradientSource::render(Surface& surface, FitMode mode) {
    if (!ready_ || !active_) return false;

    const int width = surface.pixelWidth();
    const int height = surface.pixelHeight();
    if (width <= 0 || height <= 0) return false;

    glViewport(0, 0, width, height);

    program_.use();
    // The gradient has no intrinsic size, so no fit mode applies to it — every
    // mode would produce the same picture. Passing identity rather than
    // branching keeps the shader interface the same as the video path's.
    (void)mode;
    program_.setFit(FitTransform{});
    program_.setResolution(width, height);
    program_.setTime(static_cast<float>(monotonicMs() - originMs_) / 1000.0f);
    program_.drawFullScreen();

    return true;
}

std::string GradientSource::summary() const { return format("gradient · %d fps", fps_); }

}  // namespace livewall
