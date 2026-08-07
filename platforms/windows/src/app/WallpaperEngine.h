// Ties the pieces together: one `MonitorController` per display, a shared
// `PowerMonitor`, and the current selection from the `Library`.
//
// Each display gets its own source instance, so a two-monitor setup decodes
// twice. Sharing one decoder across displays would be cheaper, but only when
// both are visible — and the common case is that one of them is covered, where
// separate sources let the covered one tear down independently. Worth
// revisiting if three-plus display setups turn out to be common.
#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "app/MonitorController.h"
#include "import/Library.h"
#include "render/D3DDevice.h"
#include "support/PowerMonitor.h"

namespace livewall {

class WallpaperEngine {
public:
    WallpaperEngine();
    ~WallpaperEngine();

    // `window` is the app's message-only window; the engine registers its
    // timers on it and the power monitor's notifications go there too.
    void start(HWND window);
    void stop();

    // Called when something the tray menu displays has changed.
    std::function<void()> onStateChange;

    Library& library() { return library_; }
    PowerMonitor& power() { return power_; }

    // Empty id selects the procedural mode.
    void select(const std::string& itemId);
    void setFitMode(FitMode mode);
    void setPauseOnBattery(bool value);

    // Forwarded from the window procedure.
    bool handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // Rebuilds every desktop window. Called when Explorer restarts, which
    // destroys the WorkerW the windows were parented into.
    void rebuildDesktopWindows();

    // ---- Status, for the tray menu ----
    bool isAnyRendering() const;
    int monitorCount() const { return static_cast<int>(controllers_.size()); }
    std::string statusLine() const;
    // The monitor the mouse is on, for the scaling tooltips.
    const MonitorController* primaryController() const;

    static constexpr UINT_PTR kVisibilityTimerId = 2;
    static constexpr UINT_PTR kDeactivateTimerId = 3;
    static constexpr UINT_PTR kDisplayChangeTimerId = 4;

private:
    void syncMonitors(bool applyToNew);
    void applySelection();
    void applySelectionTo(MonitorController& controller);
    std::unique_ptr<WallpaperSource> makeProceduralSource();
    std::unique_ptr<WallpaperSource> makeVideoSource(const WallpaperItem& item);
    void evaluateAll();

    HWND window_ = nullptr;
    Library library_;
    PowerMonitor power_;
    std::shared_ptr<D3DDevice> device_;

    std::map<HMONITOR, std::unique_ptr<MonitorController>> controllers_;

    // Set while a video source is being prepared, so the menu can say
    // "Loading…" rather than "Paused".
    int pendingPrepares_ = 0;

    // True when the selected item could not be prepared and the app fell back
    // to the procedural mode, so the menu can say so.
    bool fellBackToProcedural_ = false;
};

}  // namespace livewall
