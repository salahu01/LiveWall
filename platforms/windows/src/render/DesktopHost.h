// A borderless, click-through window pinned behind the desktop icons.
//
// This is the Windows counterpart of `DesktopWindow`, and it is the one place
// where the port does something macOS has no equivalent of, so it is worth
// stating what is going on.
//
// macOS has a documented window level for exactly this: put a window at
// `kCGDesktopWindowLevel` and it sits above the desktop picture and below the
// icons, permanently, with one API call. Windows has no window level below the
// desktop icons at all. What it has is Explorer's own window arrangement:
//
//     Progman  ("Program Manager")
//       └─ WorkerW                      ← the wallpaper is painted here
//       └─ SHELLDLL_DefView             ← the icon view
//            └─ SysListView32
//       └─ WorkerW                      ← a second, empty one
//
// Sending Progman the undocumented message 0x052C makes it split its wallpaper
// rendering into a separate WorkerW that sits *behind* SHELLDLL_DefView. A
// child window parented into that WorkerW therefore draws over the wallpaper
// and under the icons, is never a target for clicks or Alt-Tab, and does not
// appear in the window list — which is what every live-wallpaper app on Windows
// does, and there is no supported alternative.
//
// What that costs, honestly:
//
//  - It is undocumented, so it can change. The code below verifies the
//    arrangement it got rather than assuming, and falls back to parenting into
//    Progman itself, which still works but draws *over* the icons.
//  - Explorer restarting destroys the whole tree. `DesktopHost` detects the
//    dead parent and the app rebuilds; `AppHost` listens for the TaskbarCreated
//    broadcast, which is the reliable signal that it has come back.
//  - There is one WorkerW for the whole virtual desktop, not one per monitor,
//    so per-monitor windows are children positioned in virtual-screen
//    coordinates.
#pragma once

#include <windows.h>

#include <dcomp.h>
#include <wrl/client.h>

#include <memory>
#include <string>

namespace livewall {

using Microsoft::WRL::ComPtr;

class DesktopHost {
public:
    // Prepares Explorer's window tree and returns the WorkerW to parent into.
    // Null when the arrangement could not be produced, in which case
    // `fallbackParent()` names what the caller got instead.
    static HWND resolveWallpaperParent();

    // True when the last resolve had to fall back to Progman, which means the
    // wallpaper draws over the desktop icons rather than under them.
    static bool usingFallbackParent();

    // Forgets the cached parent. Called when Explorer restarts.
    static void invalidateParent();

    // Creates one window covering `monitor`. Returns null on failure.
    static std::unique_ptr<DesktopHost> create(HMONITOR monitor);

    ~DesktopHost();

    DesktopHost(const DesktopHost&) = delete;
    DesktopHost& operator=(const DesktopHost&) = delete;

    HWND window() const { return window_; }
    HMONITOR monitor() const { return monitor_; }

    // Physical pixels. Per-monitor-v2 DPI awareness means these are the real
    // panel dimensions on every display, not a virtualised 96-DPI rectangle.
    int width() const { return width_; }
    int height() const { return height_; }

    // Re-reads the monitor rectangle and moves the window. Called when displays
    // are added, removed, rearranged or have their resolution changed —
    // WM_DISPLAYCHANGE fires for all of it.
    bool updateGeometry();

    // True when the window or its parent has been destroyed under us, which in
    // practice means Explorer restarted.
    bool isOrphaned() const;

    // The DirectComposition target this monitor's swap chain is bound to.
    // Composition, rather than a plain HWND swap chain, is what allows a
    // transparent frame: in Fit mode the letterbox bars have to show the user's
    // real wallpaper rather than black, and an HWND swap chain is opaque by
    // construction.
    IDCompositionVisual* visual() const { return visual_.Get(); }
    IDCompositionDevice* compositionDevice() const { return compositionDevice_.Get(); }

    // Publishes whatever the visual currently holds. Cheap, and required after
    // the swap chain is first attached.
    void commit();

private:
    DesktopHost() = default;
    bool initialise(HMONITOR monitor);
    bool createCompositionTree();

    HWND window_ = nullptr;
    HWND parent_ = nullptr;
    HMONITOR monitor_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
};

}  // namespace livewall
