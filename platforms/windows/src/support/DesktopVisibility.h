// How much of a monitor's desktop is actually left uncovered.
//
// This is the Windows half of the macOS `DesktopVisibility`, and it carries
// more weight here. On macOS `NSWindow.occlusionState` at least answers the
// question as a yes/no and fires a notification when it changes. Windows offers
// nothing equivalent for a window parented into WorkerW — it is behind the
// desktop icons by construction, so the DWM never considers it occluded and
// never has anything to report. The window list is not a refinement of a
// cheaper signal here; it is the only signal.
//
// Only window *geometry* is read — bounds, style, cloak state and layered
// alpha. No window titles, no window contents, so nothing here needs a capture
// permission or a graphics-capture session.
#pragma once

#include <windows.h>

#include <vector>

namespace livewall {

struct Rect {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;

    double width() const { return right - left; }
    double height() const { return bottom - top; }
};

class DesktopVisibility {
public:
    // Fraction of `monitor`'s desktop not covered by an opaque top-level
    // window, 0...1. Returns 1 when the window list cannot be walked, so a
    // failure here can never be the thing that stops a wallpaper.
    static double visibleFraction(HMONITOR monitor);

    // The geometry, separated from the window-list plumbing so it can be
    // exercised without a desktop. Rectangles may overlap and may hang off the
    // edge of `bounds`; both are normal.
    static double uncoveredFraction(const Rect& bounds, const std::vector<Rect>& occluders);

    // True when a window is running exclusive full-screen or the shell has
    // declared a presentation/quiet state. Full-screen games are the single
    // biggest reason a Windows wallpaper should stop, and they do not always
    // produce a window rectangle the grid above can see — a swapchain in
    // exclusive mode may not be in the window list at all.
    static bool fullScreenAppRunning();
};

}  // namespace livewall
