// Ties the pieces together: one OutputController per display, a shared
// PowerMonitor, and the current selection from the Library.
//
// Each output gets its own source instance, so a two-monitor setup decodes
// twice. Sharing one decoder across outputs would be cheaper, but only while
// both are visible — and the common case is that one of them is covered, where
// separate sources let the covered one tear down independently. Worth
// revisiting if three-plus display setups turn out to be common.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/OutputController.h"
#include "import/Library.h"
#include "import/Transcoder.h"
#include "platform/Backend.h"
#include "support/PowerMonitor.h"

namespace livewall {

class EglDevice;

class WallpaperEngine {
public:
    WallpaperEngine(Backend& backend, EglDevice& egl);
    ~WallpaperEngine();

    // Called when something the tray or the status line displays has changed.
    std::function<void()> onStateChange;

    void start();

    // Rebuilds controllers to match the backend's current output list.
    // Existing outputs keep their controller and their decoder; only genuinely
    // new ones get a source built.
    //
    // That distinction is the whole reason this is not just "recreate
    // everything". Output-change events fire for far more than a monitor being
    // plugged in — a resolution change, a wake, a workspace with a different
    // layout — and rebuilding every source on each one tore down and recreated
    // a decoder several times a minute on the macOS port, which both churned
    // CPU and grew the process's footprint steadily.
    void syncOutputs();

    // Runs the gates and renders whatever is due. Returns how long the caller
    // may sleep, in milliseconds.
    int tick();

    void select(const std::string& itemId);
    void setFitMode(FitMode mode);
    void setPauseOnBattery(bool value);

    Library& library() { return library_; }
    PowerMonitor& power() { return power_; }

    bool isAnyRendering() const;
    size_t outputCount() const { return controllers_.size(); }

    // One line for the tray and for `livewall status`.
    std::string statusLine() const;

    // The display the transcoder should size against: the largest output, since
    // a file that covers the biggest panel covers all of them.
    DisplayTarget displayTarget() const;

private:
    // Takes the controller because preparing a source compiles shaders, and
    // that needs one of its surfaces to be current first.
    std::unique_ptr<WallpaperSource> makeSource(OutputController& target);
    void applySelection();

    Backend& backend_;
    EglDevice& egl_;
    Library library_;
    PowerMonitor power_;
    std::vector<std::unique_ptr<OutputController>> controllers_;
    FitMode fitMode_ = FitMode::Fill;

    // True when the selected item could not be prepared and the procedural mode
    // is standing in. Surfaced in the status line, because otherwise "I picked
    // a video and got a gradient" has no explanation.
    bool fellBackToProcedural_ = false;
};

}  // namespace livewall
