// The X11 backend.
//
// Three things it does that are not obvious, each of which took a wrong version
// first:
//
//   Override-redirect, lowered. The window is unmanaged and pushed to the
//   bottom of the stack, rather than a managed `_NET_WM_WINDOW_TYPE_DESKTOP`
//   window. Managed desktop windows are placed by the window manager, and most
//   window managers force them to the full screen area — which is wrong the
//   moment there is more than one monitor, because this port draws one window
//   per output. Unmanaged means the geometry is ours.
//
//   Empty input region. Without XFixes shaping the input region to nothing, the
//   window swallows clicks meant for the desktop icons and for the root menu
//   that many window managers put on a right-click.
//
//   An ARGB visual only when something can composite it. Alpha in a window with
//   no compositing manager is undefined — in practice it is opaque black, which
//   is exactly what the Fit letterbox must not be. The backend checks for a
//   compositor and asks for 32-bit only when one is there; otherwise the bars
//   are drawn black and the README says so.
#pragma once

#include <X11/Xlib.h>

#include <memory>

#include "platform/Backend.h"
#include "support/Dynamic.h"

namespace livewall {

class X11Backend final : public Backend {
public:
    // Returns null when there is no X display to open, which is not an error —
    // it is how `Backend::create` falls through to Wayland.
    static std::unique_ptr<X11Backend> open();

    ~X11Backend() override;

    const char* name() const override { return "x11"; }
    bool supportsOcclusion() const override { return true; }
    bool prefersAlpha() const override { return hasCompositor(); }

    std::vector<OutputInfo> outputs() override;
    std::unique_ptr<Surface> createSurface(const OutputInfo& output, EglDevice& egl) override;
    double visibleFraction(const OutputInfo& output) override;

    std::optional<bool> displayAsleep() override;
    std::optional<std::int64_t> idleMillis() override;

    int eventFd() override;
    bool dispatchEvents() override;
    void flush() override;

    unsigned eglPlatform() const override;
    void* nativeDisplay() const override { return display_; }

    // True when a compositing manager owns the _NET_WM_CM_Sn selection, which
    // is what makes a 32-bit visual meaningful.
    bool hasCompositor() const;

    Display* display() const { return display_; }
    int screen() const { return screen_; }
    Window root() const { return root_; }

    // Surfaces register themselves so the occlusion walk can skip them. Our own
    // wallpaper window is the thing being occluded, not an occluder.
    void registerWindow(Window window);
    void unregisterWindow(Window window);

private:
    X11Backend() = default;

    bool initialize();
    void interneAtoms();
    void loadScreenSaverExtension();

    Display* display_ = nullptr;
    int screen_ = 0;
    Window root_ = 0;

    int randrEventBase_ = 0;
    bool haveRandr_ = false;
    bool haveDpms_ = false;

    std::vector<Window> ownWindows_;

    // libXss, opened at runtime. Idle detection is the only thing it is for,
    // and a machine without it keeps every other gate.
    SharedLibrary xss_;
    struct ScreenSaverInfo;
    ScreenSaverInfo* (*xssAllocInfo_)() = nullptr;
    int (*xssQueryInfo_)(Display*, Drawable, ScreenSaverInfo*) = nullptr;
    ScreenSaverInfo* xssInfo_ = nullptr;

    struct Atoms {
        Atom clientListStacking = None;
        Atom windowType = None;
        Atom windowTypeDesktop = None;
        Atom windowTypeDock = None;
        Atom windowTypeNotification = None;
        Atom state = None;
        Atom stateHidden = None;
        Atom stateSkipTaskbar = None;
        Atom stateBelow = None;
        Atom stateSticky = None;
        Atom windowOpacity = None;
        Atom frameExtents = None;
        Atom desktop = None;
        Atom currentDesktop = None;
        Atom cardinal = None;
    } atoms_;
};

}  // namespace livewall
