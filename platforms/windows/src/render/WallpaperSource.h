// A renderer that draws into one monitor's swap chain.
//
// The contract that makes the whole app cheap, unchanged from the macOS
// version:
//
//  - `activate()` builds whatever decode/GPU resources are needed and starts
//    drawing.
//  - `deactivate()` **releases those resources** rather than merely pausing a
//    timer, and leaves the last drawn frame on screen so re-activating never
//    flashes black. A deactivated source must cost zero CPU and zero GPU.
//
// The one Windows-specific addition is `attach`, which hands over the swap
// chain: on macOS a source owns a CALayer and the window installs it, whereas
// here the window owns the composition target and the source draws into it.
#pragma once

#include <memory>
#include <string>

#include "render/D3DDevice.h"
#include "render/FitMode.h"
#include "render/SwapChainTarget.h"

namespace livewall {

class WallpaperSource {
public:
    virtual ~WallpaperSource() = default;

    // Binds the source to a target. Called once when the source is installed
    // and again whenever the swap chain is rebuilt after a resolution change.
    virtual void attach(SwapChainTarget* target, int refreshHz) = 0;

    virtual void activate() = 0;
    virtual void deactivate() = 0;
    virtual bool isActive() const = 0;

    // How the frame maps onto a display of a different aspect ratio. Procedural
    // sources draw to whatever bounds they are handed, so they have no aspect
    // ratio of their own and nothing to fit.
    virtual void setFitMode(FitMode mode) { (void)mode; }

    // Short label for the tray menu.
    virtual std::string summary() const = 0;
};

}  // namespace livewall
