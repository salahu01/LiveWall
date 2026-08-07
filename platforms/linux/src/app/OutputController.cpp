#include "app/OutputController.h"

#include <algorithm>
#include <cstdlib>

#include "render/EglDevice.h"
#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {

bool OutputController::forceRender() {
    static const bool value = std::getenv("LIVEWALL_FORCE_RENDER") != nullptr;
    return value;
}

OutputController::OutputController(OutputInfo info, Backend& backend, EglDevice& egl,
                                   PowerMonitor& power)
    : info_(std::move(info)), backend_(backend), egl_(egl), power_(power) {}

OutputController::~OutputController() {
    // The source first: it holds GL objects that need the surface's context to
    // be destroyable, and the surface's destructor takes that context away.
    if (source_) source_->deactivate();
    source_.reset();
    surface_.reset();
}

bool OutputController::createSurface() {
    surface_ = backend_.createSurface(info_, egl_);
    return surface_ != nullptr;
}

bool OutputController::makeContextCurrent() {
    return surface_ && surface_->makeCurrent();
}

void OutputController::setSource(std::unique_ptr<WallpaperSource> source) {
    if (source_) source_->deactivate();
    deactivateAtMs_ = 0;

    source_ = std::move(source);
    nextFrameMs_ = 0;
    nextVisibilityPollMs_ = 0;
}

void OutputController::updateInfo(const OutputInfo& info) {
    info_ = info;
    if (surface_) surface_->resize(info);
}

bool OutputController::isRendering() const { return source_ && source_->isActive(); }

std::string OutputController::sourceSummary() const {
    return source_ ? source_->summary() : "none";
}

bool OutputController::updateCoverageLatch() {
    if (!backend_.supportsOcclusion()) return false;

    const double fraction = backend_.visibleFraction(info_);
    const bool was = nearlyCovered_;

    if (nearlyCovered_) {
        if (fraction >= kResumeAboveFraction) nearlyCovered_ = false;
    } else if (fraction < kStopBelowFraction) {
        nearlyCovered_ = true;
    }

    if (nearlyCovered_ != was) {
        Log::info(format("%s: desktop %.0f%% visible — %s", info_.id.c_str(), fraction * 100,
                         nearlyCovered_ ? "stopping" : "resuming"));
        return true;
    }
    return false;
}

void OutputController::evaluate(std::int64_t nowMs) {
    if (!source_ || !surface_) return;

    const bool systemBlocked = power_.systemBlocksRendering();

    // The expensive signal — walking the window list — is paid for only when it
    // can change the answer. If the machine is blocking anyway, we are already
    // stopped and something will wake us when that changes.
    if (!systemBlocked && nowMs >= nextVisibilityPollMs_) {
        nextVisibilityPollMs_ = nowMs + kVisibilityPollMs;
        updateCoverageLatch();
    }

    const bool shouldRender = forceRender() || (!systemBlocked && !nearlyCovered_);

    if (shouldRender) {
        deactivateAtMs_ = 0;
        if (!source_->isActive()) {
            source_->activate();
            // Draw the first frame immediately rather than on the next tick, so
            // resuming from occlusion is not visibly a frame late.
            nextFrameMs_ = nowMs;
        }
        return;
    }

    if (!source_->isActive()) return;

    if (deactivateAtMs_ == 0) {
        deactivateAtMs_ = nowMs + kDeactivateDelayMs;
        return;
    }
    if (nowMs >= deactivateAtMs_) {
        deactivateAtMs_ = 0;
        // Re-checked rather than trusted: the 400 ms is there precisely because
        // the condition often reverses inside it, and tearing down a decoder
        // that should be running is the expensive mistake.
        const bool stillBlocked = power_.systemBlocksRendering() || nearlyCovered_;
        if (stillBlocked && !forceRender()) source_->deactivate();
    }
}

bool OutputController::tick(std::int64_t nowMs, FitMode mode) {
    if (!source_ || !surface_ || !source_->isActive()) return false;
    if (!surface_->isReady()) return false;
    if (nowMs < nextFrameMs_) return false;

    // Wayland's frame callback. When the compositor has stopped asking, this is
    // the only hint that nothing can see the surface — so the pump stalls here
    // rather than decoding frames for a hidden output.
    if (!surface_->readyForFrame()) return false;

    if (!surface_->makeCurrent()) return false;

    const int fps = std::max(1, source_->framesPerSecond());
    // Scheduled from the deadline rather than from now, so a tick that ran late
    // does not push every subsequent one late as well.
    const std::int64_t interval = 1000 / fps;
    nextFrameMs_ = std::max(nowMs + 1, nextFrameMs_ + interval);
    // A long stall — the machine suspended, or the loop was blocked on a slow
    // DBus call — would otherwise leave the deadline far in the past and cause
    // a burst of catch-up frames that plays the clip fast.
    if (nextFrameMs_ < nowMs) nextFrameMs_ = nowMs + interval;

    if (!source_->render(*surface_, mode)) return false;

    surface_->present();
    return true;
}

std::int64_t OutputController::nextDeadlineMs() const {
    std::int64_t deadline = nextVisibilityPollMs_;

    if (source_ && source_->isActive()) {
        deadline = deadline == 0 ? nextFrameMs_ : std::min(deadline, nextFrameMs_);
    }
    if (deactivateAtMs_ != 0) {
        deadline = deadline == 0 ? deactivateAtMs_ : std::min(deadline, deactivateAtMs_);
    }
    return deadline;
}

}  // namespace livewall
