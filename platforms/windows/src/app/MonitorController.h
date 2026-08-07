// Owns one monitor's desktop window, its swap chain and the source drawing
// into it, and decides moment to moment whether that source should run at all.
//
// Rendering is allowed only when both hold:
//   - enough of this monitor's desktop is uncovered, and
//   - no machine-wide condition blocks it (lock, display off, Battery Saver…).
//
// Deactivation is delayed slightly so that transient coverage during window
// animations, Task View or a Win+Tab does not tear down and rebuild the decoder
// several times a second. Activation is immediate — there is no reason to make
// the user wait to see their wallpaper.
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include "render/DesktopHost.h"
#include "render/SwapChainTarget.h"
#include "render/WallpaperSource.h"
#include "support/PowerMonitor.h"

namespace livewall {

class MonitorController {
public:
    static std::unique_ptr<MonitorController> create(HMONITOR monitor,
                                                     std::shared_ptr<D3DDevice> device,
                                                     PowerMonitor* power);

    ~MonitorController();

    MonitorController(const MonitorController&) = delete;
    MonitorController& operator=(const MonitorController&) = delete;

    HMONITOR monitor() const { return monitor_; }

    // Fired when this monitor's own gating changed, so the tray menu can
    // reflect a stop that no system notification announced.
    std::function<void()> onStateChange;

    void setSource(std::unique_ptr<WallpaperSource> source);
    void setFitMode(FitMode mode);

    // Re-reads the monitor rectangle. Rebuilds the swap chain only when the
    // resolution actually changed — WM_DISPLAYCHANGE fires for far more than
    // that, and rebuilding on each one is what made the macOS version churn
    // decoders several times a minute before it learned to tell moves from
    // resizes.
    void refreshGeometry();

    // Recomputes whether the source should be running and acts on it.
    void evaluate();

    // The delayed teardown fired. Called by the owner's timer.
    void deactivateIfStillBlocked();

    bool isRendering() const;
    bool isNearlyCovered() const { return desktopNearlyCovered_; }
    bool isOrphaned() const { return host_ == nullptr || host_->isOrphaned(); }
    std::string sourceSummary() const;

    int width() const { return host_ != nullptr ? host_->width() : 0; }
    int height() const { return host_ != nullptr ? host_->height() : 0; }
    int refreshHz() const { return refreshHz_; }

    // True while a delayed deactivation is pending, so the owner knows whether
    // its timer still has work to do.
    bool hasPendingDeactivate() const { return pendingDeactivate_; }

    // Stop below this much of the desktop left uncovered, resume once this much
    // is back. The gap between the two is deliberate: a single threshold makes
    // a window edge resting near it toggle the decoder repeatedly.
    static constexpr double kStopBelowFraction = 0.08;
    static constexpr double kResumeAboveFraction = 0.15;

    // Windows can be moved and resized without any notification we can observe,
    // so the uncovered fraction has to be sampled. Walking the window list is
    // not free — on the macOS side profiling put the equivalent poll at more
    // main-thread samples than the entire frame pump — so it happens rarely: a
    // wallpaper that takes four seconds to notice it has been covered has cost
    // nothing anyone can perceive.
    static constexpr UINT kVisibilityPollMs = 4000;

    // Long enough to sit out a window animation or a Task View sweep, short
    // enough that a genuinely covered desktop stops promptly.
    static constexpr UINT kDeactivateDelayMs = 400;

private:
    MonitorController() = default;
    bool initialise(HMONITOR monitor, std::shared_ptr<D3DDevice> device, PowerMonitor* power);
    bool updateCoverageLatch();
    void readRefreshRate();

    HMONITOR monitor_ = nullptr;
    std::shared_ptr<D3DDevice> device_;
    PowerMonitor* power_ = nullptr;

    std::unique_ptr<DesktopHost> host_;
    std::unique_ptr<SwapChainTarget> target_;
    std::unique_ptr<WallpaperSource> source_;

    int refreshHz_ = 60;

    // Latched through the hysteresis band rather than recomputed as a plain
    // comparison each time.
    bool desktopNearlyCovered_ = false;
    bool pendingDeactivate_ = false;
};

}  // namespace livewall
