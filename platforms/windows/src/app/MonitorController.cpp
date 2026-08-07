#include "app/MonitorController.h"

#include <iterator>

#include "support/DesktopVisibility.h"
#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {
namespace {

// Keeps rendering even when the desktop is covered. Exists so playback cost can
// be measured on a machine whose desktop is behind a terminal — the coverage
// gate otherwise tears the decoder down and every reading comes back as the
// idle baseline. Never set in normal use.
bool forceRender() {
    static const bool value = [] {
        wchar_t buffer[8]{};
        return GetEnvironmentVariableW(L"LIVEWALL_FORCE_RENDER", buffer,
                                       static_cast<DWORD>(std::size(buffer))) > 0;
    }();
    return value;
}

}  // namespace

std::unique_ptr<MonitorController> MonitorController::create(HMONITOR monitor,
                                                             std::shared_ptr<D3DDevice> device,
                                                             PowerMonitor* power) {
    std::unique_ptr<MonitorController> controller(new MonitorController());
    if (!controller->initialise(monitor, std::move(device), power)) return nullptr;
    return controller;
}

MonitorController::~MonitorController() {
    // Order matters: the source's render thread touches the swap chain, so it
    // has to be stopped before the target it draws into goes away.
    source_.reset();
    target_.reset();
    host_.reset();
}

bool MonitorController::initialise(HMONITOR monitor, std::shared_ptr<D3DDevice> device,
                                   PowerMonitor* power) {
    monitor_ = monitor;
    device_ = std::move(device);
    power_ = power;

    host_ = DesktopHost::create(monitor_);
    if (!host_) return false;

    target_ = SwapChainTarget::create(device_, *host_);
    if (!target_) return false;

    readRefreshRate();
    return true;
}

void MonitorController::readRefreshRate() {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor_, &info) == 0) return;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) != 0 &&
        mode.dmDisplayFrequency > 1) {
        refreshHz_ = static_cast<int>(mode.dmDisplayFrequency);
    }
}

void MonitorController::setSource(std::unique_ptr<WallpaperSource> source) {
    pendingDeactivate_ = false;

    // The outgoing source's thread has to be joined before the incoming one
    // starts drawing, or two threads present into the same swap chain.
    if (source_) source_->deactivate();
    source_ = std::move(source);

    if (!source_) return;

    source_->attach(target_.get(), refreshHz_);
    evaluate();
}

void MonitorController::setFitMode(FitMode mode) {
    if (source_) source_->setFitMode(mode);
}

void MonitorController::refreshGeometry() {
    if (!host_) return;

    const bool resized = host_->updateGeometry();
    if (!resized) {
        // A pure move needs nothing rebuilt: the window has already been
        // repositioned and the swap chain's buffers are still the right size.
        readRefreshRate();
        return;
    }

    const bool wasActive = isRendering();
    if (source_) source_->deactivate();

    readRefreshRate();
    if (target_) target_->resize(host_->width(), host_->height());
    if (source_) source_->attach(target_.get(), refreshHz_);

    if (wasActive) evaluate();
}

bool MonitorController::updateCoverageLatch() {
    const double fraction = DesktopVisibility::visibleFraction(monitor_);
    const bool wasNearlyCovered = desktopNearlyCovered_;

    if (desktopNearlyCovered_) {
        if (fraction >= kResumeAboveFraction) desktopNearlyCovered_ = false;
    } else if (fraction < kStopBelowFraction) {
        desktopNearlyCovered_ = true;
    }

    if (desktopNearlyCovered_ != wasNearlyCovered) {
        Log::info(format("desktop %.0f%% visible — %s", fraction * 100,
                         desktopNearlyCovered_ ? "stopping" : "resuming"));
        return true;
    }
    return false;
}

void MonitorController::evaluate() {
    if (!source_) return;

    const bool systemBlocked = power_ != nullptr && power_->systemBlocksRendering();

    // Only pay for the window-list walk when it can change the answer. If the
    // machine is blocking anyway we are already stopped, and a notification
    // will wake us when that changes.
    const bool latchChanged = systemBlocked ? false : updateCoverageLatch();

    const bool shouldRender =
        (!desktopNearlyCovered_ && !systemBlocked) || forceRender();

    if (latchChanged && onStateChange) onStateChange();

    if (shouldRender) {
        pendingDeactivate_ = false;
        if (!source_->isActive()) {
            source_->activate();
            if (onStateChange) onStateChange();
        }
        return;
    }

    // Delay the teardown. A window animation, a Task View sweep or a Win+Tab
    // covers the desktop for a few hundred milliseconds, and rebuilding the
    // decoder for each one costs more than leaving it running would have.
    if (source_->isActive() && !pendingDeactivate_) pendingDeactivate_ = true;
}

void MonitorController::deactivateIfStillBlocked() {
    if (!pendingDeactivate_) return;
    pendingDeactivate_ = false;

    if (!source_ || !source_->isActive()) return;
    if (forceRender()) return;

    const bool systemBlocked = power_ != nullptr && power_->systemBlocksRendering();
    if (!systemBlocked) updateCoverageLatch();

    if (desktopNearlyCovered_ || systemBlocked) {
        source_->deactivate();
        if (onStateChange) onStateChange();
    }
}

bool MonitorController::isRendering() const {
    return source_ != nullptr && source_->isActive();
}

std::string MonitorController::sourceSummary() const {
    return source_ != nullptr ? source_->summary() : "none";
}

}  // namespace livewall
