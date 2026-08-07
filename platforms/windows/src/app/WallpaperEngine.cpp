#include "app/WallpaperEngine.h"

#include <algorithm>
#include <vector>

#include "render/GradientSource.h"
#include "render/VideoSource.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* found = reinterpret_cast<std::vector<HMONITOR>*>(parameter);
    found->push_back(monitor);
    return TRUE;
}

std::vector<HMONITOR> currentMonitors() {
    std::vector<HMONITOR> found;
    EnumDisplayMonitors(nullptr, nullptr, &collectMonitor, reinterpret_cast<LPARAM>(&found));
    return found;
}

}  // namespace

WallpaperEngine::WallpaperEngine() = default;

WallpaperEngine::~WallpaperEngine() { stop(); }

void WallpaperEngine::start(HWND window) {
    window_ = window;

    device_ = D3DDevice::acquire();
    if (!device_) {
        Log::error("no usable Direct3D 11 device — nothing can be drawn");
        return;
    }

    power_.setPauseOnBattery(library_.pauseOnBattery());
    power_.onChange = [this] {
        evaluateAll();
        if (onStateChange) onStateChange();
    };
    power_.start(window_);

    syncMonitors(/*applyToNew=*/false);
    applySelection();

    // The coverage poll. Windows can be moved and resized with no notification
    // anyone can observe, so this is the only way to learn that the desktop has
    // been covered by a window that was already open.
    SetTimer(window_, kVisibilityTimerId, MonitorController::kVisibilityPollMs, nullptr);
}

void WallpaperEngine::stop() {
    if (window_ != nullptr) {
        KillTimer(window_, kVisibilityTimerId);
        KillTimer(window_, kDeactivateTimerId);
        KillTimer(window_, kDisplayChangeTimerId);
    }
    power_.stop();
    controllers_.clear();
    device_.reset();
    window_ = nullptr;
}

void WallpaperEngine::syncMonitors(bool applyToNew) {
    if (!device_) return;

    const std::vector<HMONITOR> present = currentMonitors();

    std::vector<MonitorController*> added;
    for (const HMONITOR monitor : present) {
        const auto existing = controllers_.find(monitor);
        if (existing != controllers_.end()) {
            existing->second->refreshGeometry();
            continue;
        }

        auto controller = MonitorController::create(monitor, device_, &power_);
        if (!controller) {
            Log::error("could not build a desktop window for a monitor");
            continue;
        }
        controller->onStateChange = [this] {
            if (onStateChange) onStateChange();
        };
        MonitorController* raw = controller.get();
        controllers_.emplace(monitor, std::move(controller));
        added.push_back(raw);
    }

    // Drop controllers for monitors that are gone. HMONITOR handles are reused
    // by the OS, so identity has to come from the current enumeration rather
    // than from anything cached.
    for (auto it = controllers_.begin(); it != controllers_.end();) {
        const bool stillPresent =
            std::find(present.begin(), present.end(), it->first) != present.end();
        it = stillPresent ? std::next(it) : controllers_.erase(it);
    }

    if (applyToNew) {
        for (MonitorController* controller : added) applySelectionTo(*controller);
    }
}

void WallpaperEngine::rebuildDesktopWindows() {
    Log::info("Explorer restarted — rebuilding the desktop windows");

    // The WorkerW every window was parented into is gone, and so is the cached
    // handle for it.
    controllers_.clear();
    DesktopHost::invalidateParent();

    syncMonitors(/*applyToNew=*/false);
    applySelection();
}

std::unique_ptr<WallpaperSource> WallpaperEngine::makeProceduralSource() {
    return std::make_unique<GradientSource>(device_);
}

std::unique_ptr<WallpaperSource> WallpaperEngine::makeVideoSource(const WallpaperItem& item) {
    auto source = std::make_unique<VideoSource>(device_, library_.pathFor(item), item.fps,
                                                item.pixelBitDepth(), library_.fitMode());
    ++pendingPrepares_;
    const bool ok = source->prepare();
    --pendingPrepares_;

    if (!ok) return nullptr;
    return source;
}

void WallpaperEngine::applySelection() {
    for (auto& [monitor, controller] : controllers_) applySelectionTo(*controller);
    if (onStateChange) onStateChange();
}

void WallpaperEngine::applySelectionTo(MonitorController& controller) {
    fellBackToProcedural_ = false;

    const std::string id = library_.selectedId();
    if (id.empty()) {
        controller.setSource(makeProceduralSource());
        return;
    }

    const WallpaperItem* item = library_.item(id);
    if (item == nullptr || !paths::fileExists(library_.pathFor(*item))) {
        Log::error("selected wallpaper missing on disk, falling back to procedural");
        library_.setSelectedId({});
        fellBackToProcedural_ = true;
        controller.setSource(makeProceduralSource());
        return;
    }

    auto video = makeVideoSource(*item);
    if (!video) {
        Log::error("could not prepare " + item->title + "; falling back to procedural");
        fellBackToProcedural_ = true;
        controller.setSource(makeProceduralSource());
        return;
    }
    controller.setSource(std::move(video));
}

void WallpaperEngine::select(const std::string& itemId) {
    library_.setSelectedId(itemId);
    applySelection();
}

void WallpaperEngine::setFitMode(FitMode mode) {
    library_.setFitMode(mode);
    // Applied to live sources in place — no reload, no decode interruption.
    for (auto& [monitor, controller] : controllers_) controller->setFitMode(mode);
    if (onStateChange) onStateChange();
}

void WallpaperEngine::setPauseOnBattery(bool value) {
    library_.setPauseOnBattery(value);
    power_.setPauseOnBattery(value);
    if (onStateChange) onStateChange();
}

void WallpaperEngine::evaluateAll() {
    bool anyPending = false;
    for (auto& [monitor, controller] : controllers_) {
        controller->evaluate();
        anyPending = anyPending || controller->hasPendingDeactivate();
    }

    // A single one-shot timer for every pending teardown rather than one per
    // monitor. They all want the same delay, and a timer per display on a
    // six-monitor machine is six wake-ups where one will do.
    if (anyPending && window_ != nullptr) {
        SetTimer(window_, kDeactivateTimerId, MonitorController::kDeactivateDelayMs, nullptr);
    }
}

bool WallpaperEngine::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_TIMER:
            if (wParam == kVisibilityTimerId) {
                evaluateAll();
                return true;
            }
            if (wParam == kDeactivateTimerId) {
                KillTimer(window_, kDeactivateTimerId);
                for (auto& [monitor, controller] : controllers_) {
                    controller->deactivateIfStillBlocked();
                }
                return true;
            }
            if (wParam == kDisplayChangeTimerId) {
                KillTimer(window_, kDisplayChangeTimerId);
                syncMonitors(/*applyToNew=*/true);
                evaluateAll();
                if (onStateChange) onStateChange();
                return true;
            }
            return false;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            // Coalesced through a short timer. A resolution change or a monitor
            // being plugged in produces a burst of these — one per display, plus
            // more as the shell rearranges — and reacting to each would tear
            // every decoder down and rebuild it several times over.
            if (window_ != nullptr) SetTimer(window_, kDisplayChangeTimerId, 500, nullptr);
            return true;

        case WM_SETTINGCHANGE:
            // Covers a work-area change (taskbar moved or auto-hidden) as well
            // as the screen-saver settings the power monitor watches for.
            if (window_ != nullptr) SetTimer(window_, kDisplayChangeTimerId, 500, nullptr);
            return false;  // the power monitor wants this one too

        default:
            return false;
    }
}

bool WallpaperEngine::isAnyRendering() const {
    for (const auto& [monitor, controller] : controllers_) {
        if (controller->isRendering()) return true;
    }
    return false;
}

const MonitorController* WallpaperEngine::primaryController() const {
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);

    const auto it = controllers_.find(monitor);
    if (it != controllers_.end()) return it->second.get();
    return controllers_.empty() ? nullptr : controllers_.begin()->second.get();
}

std::string WallpaperEngine::statusLine() const {
    if (const auto reason = power_.blockReason()) return "Paused — " + *reason;
    if (pendingPrepares_ > 0) return "Loading…";
    if (controllers_.empty()) return "No displays";
    if (isAnyRendering()) {
        // Worth saying out loud: the user picked a video and is looking at a
        // gradient, and without this the menu would claim everything is fine.
        return fellBackToProcedural_ ? "Rendering — fell back to procedural" : "Rendering";
    }

    for (const auto& [monitor, controller] : controllers_) {
        if (controller->isNearlyCovered()) return "Paused — desktop nearly covered";
    }
    return "Paused — desktop covered";
}

}  // namespace livewall
