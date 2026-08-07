#include "support/DesktopVisibility.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <vector>

#include "support/Log.h"

#pragma comment(lib, "dwmapi.lib")

namespace livewall {
namespace {

// Coverage is measured by marking cells on a grid rather than computing an
// exact rectangle union. The answer feeds a threshold comparison, so ~1%
// precision is ample and this stays a few hundred integer operations against
// code that would otherwise need a full sweep-line. Same grid as the macOS
// side, so the two report the same number for the same arrangement.
constexpr int kGridColumns = 64;
constexpr int kGridRows = 40;

struct EnumContext {
    RECT monitorBounds{};
    HMONITOR monitor = nullptr;
    DWORD ownProcess = 0;
    std::vector<Rect> occluders;
};

Rect toRect(const RECT& r) {
    return Rect{static_cast<double>(r.left), static_cast<double>(r.top),
                static_cast<double>(r.right), static_cast<double>(r.bottom)};
}

bool isCloaked(HWND window) {
    // A cloaked window is present in the window list and reports IsWindowVisible
    // true, but the DWM is not drawing it: the classic case is a UWP app
    // suspended in the background, and there are usually several of them. They
    // are full-screen-sized, so counting them charged the desktop as covered on
    // a machine where nothing was covering anything.
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return false;
    }
    return cloaked != 0;
}

bool isOpaqueEnough(HWND window, LONG exStyle) {
    if ((exStyle & WS_EX_LAYERED) == 0) return true;

    BYTE alpha = 255;
    DWORD flags = 0;
    if (GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) == 0) {
        // A layered window using UpdateLayeredWindow rather than
        // SetLayeredWindowAttributes has per-pixel alpha we cannot read. Those
        // are overlays and skins — treat them as see-through, which
        // over-estimates what is visible and therefore keeps rendering. That
        // is the safe direction to be wrong.
        return false;
    }
    if ((flags & LWA_ALPHA) == 0) return true;
    return alpha >= 242;  // ~0.95, matching the macOS threshold.
}

BOOL CALLBACK enumerate(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<EnumContext*>(parameter);

    if (IsWindowVisible(window) == 0) return TRUE;
    if (IsIconic(window) != 0) return TRUE;

    // Our own desktop windows are the thing being occluded, not occluders.
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == context->ownProcess) return TRUE;

    const LONG style = GetWindowLongW(window, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(window, GWL_EXSTYLE);

    // WS_EX_TOOLWINDOW is the closest Windows analogue to the macOS window
    // layers above 0: floating palettes, the tray flyouts, the taskbar's own
    // furniture. Counting the taskbar charged a permanent few percent of every
    // display against the visible budget for something no user thinks of as
    // covering their desktop.
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) return TRUE;
    // WS_EX_TRANSPARENT is click-through, which in practice means a hit-test
    // overlay with nothing painted in it.
    if ((exStyle & WS_EX_TRANSPARENT) != 0) return TRUE;
    if ((style & WS_CHILD) != 0) return TRUE;

    if (isCloaked(window)) return TRUE;
    if (!isOpaqueEnough(window, exStyle)) return TRUE;

    // Anything not on this monitor is somebody else's problem — each display
    // gates independently.
    if (MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != context->monitor &&
        MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) != context->monitor) {
        return TRUE;
    }

    // The extended frame bounds, not GetWindowRect. On Windows 10 and 11 a
    // window's rect includes the invisible resize border the DWM draws outside
    // the visible frame — typically 7 or 8 pixels a side. Over a grid this
    // reads as more coverage than there is, and on the last sliver of a nearly
    // covered desktop that is exactly the region the threshold cares about.
    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds,
                                     sizeof(bounds))) &&
        GetWindowRect(window, &bounds) == 0) {
        return TRUE;
    }

    RECT clipped{};
    if (IntersectRect(&clipped, &bounds, &context->monitorBounds) == 0) return TRUE;

    context->occluders.push_back(toRect(clipped));
    return TRUE;
}

}  // namespace

double DesktopVisibility::visibleFraction(HMONITOR monitor) {
    if (monitor == nullptr) return 1.0;

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == 0) return 1.0;

    // rcMonitor rather than rcWork: the work area excludes the taskbar, and the
    // wallpaper is drawn behind the taskbar as well. Excluding it here while
    // rendering there would make a maximised window on a 1080p display read as
    // covering more than 100% of what we draw.
    EnumContext context;
    context.monitorBounds = info.rcMonitor;
    context.monitor = monitor;
    context.ownProcess = GetCurrentProcessId();
    context.occluders.reserve(24);

    if (EnumWindows(&enumerate, reinterpret_cast<LPARAM>(&context)) == 0 &&
        GetLastError() != ERROR_SUCCESS) {
        return 1.0;
    }

    return uncoveredFraction(toRect(info.rcMonitor), context.occluders);
}

double DesktopVisibility::uncoveredFraction(const Rect& bounds,
                                            const std::vector<Rect>& occluders) {
    if (bounds.width() <= 0 || bounds.height() <= 0) return 1.0;

    bool covered[kGridColumns * kGridRows] = {};
    const double cellWidth = bounds.width() / kGridColumns;
    const double cellHeight = bounds.height() / kGridRows;

    for (const Rect& rect : occluders) {
        // Clip to the display. Rectangles hanging off the edge are normal —
        // a window straddling two monitors is the usual case.
        const Rect clipped{std::max(rect.left, bounds.left), std::max(rect.top, bounds.top),
                           std::min(rect.right, bounds.right),
                           std::min(rect.bottom, bounds.bottom)};
        if (clipped.width() <= 0 || clipped.height() <= 0) continue;

        for (int row = 0; row < kGridRows; ++row) {
            // A cell counts as covered when its centre is inside an occluder,
            // so a window edge cutting through a cell rounds to the side it
            // covers most.
            const double centreY = bounds.top + (row + 0.5) * cellHeight;
            if (centreY < clipped.top || centreY >= clipped.bottom) continue;
            for (int column = 0; column < kGridColumns; ++column) {
                const double centreX = bounds.left + (column + 0.5) * cellWidth;
                if (centreX < clipped.left || centreX >= clipped.right) continue;
                covered[row * kGridColumns + column] = true;
            }
        }
    }

    int coveredCells = 0;
    for (const bool cell : covered) coveredCells += cell ? 1 : 0;
    return 1.0 - static_cast<double>(coveredCells) / (kGridColumns * kGridRows);
}

bool DesktopVisibility::fullScreenAppRunning() {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) return false;

    switch (state) {
        // A full-screen Direct3D application — the one case where the window
        // list may not describe what is on screen at all, because an exclusive
        // swapchain bypasses the desktop compositor.
        case QUNS_RUNNING_D3D_FULL_SCREEN:
        // A full-screen window that has asked not to be interrupted, which in
        // practice is every borderless-full-screen game.
        case QUNS_BUSY:
        // Presentation mode: someone is projecting, and a moving wallpaper on
        // the desktop behind their slides is the last thing they want.
        case QUNS_PRESENTATION_MODE:
            return true;
        default:
            return false;
    }
}

}  // namespace livewall
