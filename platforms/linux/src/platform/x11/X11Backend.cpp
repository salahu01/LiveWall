#include "platform/x11/X11Backend.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/shape.h>  // ShapeInput, for the empty input region

#include <algorithm>
#include <cstring>

#include "render/EglDevice.h"
#include "support/DesktopVisibility.h"
#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {
namespace {

constexpr unsigned long kAllDesktops = 0xFFFFFFFFul;

// Reads a window property in one call, returning it as a vector of the caller's
// item type. XGetWindowProperty's five out-parameters are the same five every
// time and getting one of them wrong is silent.
template <typename T>
std::vector<T> windowProperty(Display* display, Window window, Atom property, Atom type) {
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long count = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;

    const int status = XGetWindowProperty(display, window, property, 0, 1024, False, type,
                                          &actualType, &actualFormat, &count, &bytesAfter, &data);
    std::vector<T> values;
    if (status == Success && data != nullptr && actualType != None) {
        // Format 32 means "long", not "32 bits" — on LP64 each item occupies
        // eight bytes in the returned buffer. Reading it as uint32_t is the
        // classic way to get every other value.
        const auto* items = reinterpret_cast<const unsigned long*>(data);
        for (unsigned long i = 0; i < count; ++i) values.push_back(static_cast<T>(items[i]));
    }
    if (data != nullptr) XFree(data);
    return values;
}

// X error handler. Windows disappear between being listed and being queried —
// that is a race no amount of care removes, because the list is a snapshot —
// and the default handler's response to the resulting BadWindow is to exit the
// process.
int ignoreXError(Display* display, XErrorEvent* error) {
    if (Log::verbose()) {
        char text[256] = {};
        XGetErrorText(display, error->error_code, text, sizeof(text));
        Log::info(std::string("X error ignored: ") + text);
    }
    return 0;
}

}  // namespace

// The layout libXss returns. Declared here rather than including
// <X11/extensions/scrnsaver.h> so that the header is not a build dependency
// for a library that is opened at runtime and may not be installed at all.
// This structure has been ABI-stable since the extension shipped in 1992.
struct X11Backend::ScreenSaverInfo {
    Window window;
    int state;
    int kind;
    unsigned long tilOrSince;
    unsigned long idle;
    unsigned long eventMask;
};

std::unique_ptr<X11Backend> X11Backend::open() {
    std::unique_ptr<X11Backend> backend(new X11Backend());
    if (!backend->initialize()) return nullptr;
    return backend;
}

bool X11Backend::initialize() {
    display_ = XOpenDisplay(nullptr);
    if (display_ == nullptr) {
        Log::info("no X display");
        return false;
    }

    XSetErrorHandler(ignoreXError);

    screen_ = DefaultScreen(display_);
    root_ = RootWindow(display_, screen_);

    interneAtoms();

    int randrErrorBase = 0;
    haveRandr_ = XRRQueryExtension(display_, &randrEventBase_, &randrErrorBase) == True;
    if (haveRandr_) {
        // Monitor hotplug, resolution changes and rotation all arrive here.
        // Without it the app would keep drawing a 1920-wide surface on a
        // display that had become 3840 wide.
        XRRSelectInput(display_, root_, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                                            RROutputChangeNotifyMask);
    } else {
        Log::info("no RandR — treating the X screen as a single output");
    }

    int dpmsEventBase = 0;
    int dpmsErrorBase = 0;
    haveDpms_ = DPMSQueryExtension(display_, &dpmsEventBase, &dpmsErrorBase) == True &&
                DPMSCapable(display_) == True;

    int fixesEventBase = 0;
    int fixesErrorBase = 0;
    if (XFixesQueryExtension(display_, &fixesEventBase, &fixesErrorBase) != True) {
        // Not fatal, but worth being loud about: without input shaping the
        // wallpaper eats every click on the desktop.
        Log::error("no XFixes — the wallpaper window will swallow clicks on the desktop");
    }

    loadScreenSaverExtension();
    return true;
}

void X11Backend::interneAtoms() {
    auto intern = [this](const char* name) { return XInternAtom(display_, name, False); };

    atoms_.clientListStacking = intern("_NET_CLIENT_LIST_STACKING");
    atoms_.windowType = intern("_NET_WM_WINDOW_TYPE");
    atoms_.windowTypeDesktop = intern("_NET_WM_WINDOW_TYPE_DESKTOP");
    atoms_.windowTypeDock = intern("_NET_WM_WINDOW_TYPE_DOCK");
    atoms_.windowTypeNotification = intern("_NET_WM_WINDOW_TYPE_NOTIFICATION");
    atoms_.state = intern("_NET_WM_STATE");
    atoms_.stateHidden = intern("_NET_WM_STATE_HIDDEN");
    atoms_.stateSkipTaskbar = intern("_NET_WM_STATE_SKIP_TASKBAR");
    atoms_.stateBelow = intern("_NET_WM_STATE_BELOW");
    atoms_.stateSticky = intern("_NET_WM_STATE_STICKY");
    atoms_.windowOpacity = intern("_NET_WM_WINDOW_OPACITY");
    atoms_.frameExtents = intern("_NET_FRAME_EXTENTS");
    atoms_.desktop = intern("_NET_WM_DESKTOP");
    atoms_.currentDesktop = intern("_NET_CURRENT_DESKTOP");
    atoms_.cardinal = XA_CARDINAL;
}

void X11Backend::loadScreenSaverExtension() {
    if (!xss_.open("libXss", {"libXss.so.1"})) return;

    xss_.bind(xssAllocInfo_, "XScreenSaverAllocInfo");
    xss_.bind(xssQueryInfo_, "XScreenSaverQueryInfo");
    if (!xss_.complete()) {
        Log::info("libXss is missing " + xss_.missingSymbol() + " — idle detection is off");
        xssAllocInfo_ = nullptr;
        xssQueryInfo_ = nullptr;
        return;
    }
    xssInfo_ = xssAllocInfo_();
}

X11Backend::~X11Backend() {
    if (xssInfo_ != nullptr) XFree(xssInfo_);
    if (display_ != nullptr) XCloseDisplay(display_);
}

unsigned X11Backend::eglPlatform() const { return EGL_PLATFORM_X11_KHR; }

bool X11Backend::hasCompositor() const {
    // The compositing manager owns a selection named for the screen. This is
    // the check the EWMH spec defines and the one every toolkit uses.
    const std::string name = "_NET_WM_CM_S" + std::to_string(screen_);
    const Atom selection = XInternAtom(display_, name.c_str(), False);
    return XGetSelectionOwner(display_, selection) != None;
}

void X11Backend::registerWindow(Window window) { ownWindows_.push_back(window); }

void X11Backend::unregisterWindow(Window window) {
    ownWindows_.erase(std::remove(ownWindows_.begin(), ownWindows_.end(), window),
                      ownWindows_.end());
}

std::vector<OutputInfo> X11Backend::outputs() {
    std::vector<OutputInfo> found;

    if (haveRandr_) {
        // GetScreenResourcesCurrent, not GetScreenResources: the latter forces
        // the server to re-probe every connector, which takes tens of
        // milliseconds and makes the display flicker on some drivers. This is
        // called on every hotplug event.
        XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display_, root_);
        if (resources != nullptr) {
            for (int i = 0; i < resources->noutput; ++i) {
                XRROutputInfo* info = XRRGetOutputInfo(display_, resources, resources->outputs[i]);
                if (info == nullptr) continue;

                if (info->connection != RR_Connected || info->crtc == None) {
                    XRRFreeOutputInfo(info);
                    continue;
                }

                XRRCrtcInfo* crtc = XRRGetCrtcInfo(display_, resources, info->crtc);
                if (crtc != nullptr && crtc->width > 0 && crtc->height > 0) {
                    OutputInfo output;
                    output.id.assign(info->name, static_cast<size_t>(info->nameLen));
                    output.x = crtc->x;
                    output.y = crtc->y;
                    output.width = static_cast<int>(crtc->width);
                    output.height = static_cast<int>(crtc->height);
                    // X11 has no per-output scale factor. Toolkits derive one
                    // from DPI or from an environment variable; neither is
                    // something the server knows, so the buffer is the CRTC.
                    output.pixelWidth = output.width;
                    output.pixelHeight = output.height;
                    output.scale = 1.0;

                    for (int m = 0; m < resources->nmode; ++m) {
                        const XRRModeInfo& mode = resources->modes[m];
                        if (mode.id != crtc->mode) continue;
                        const double total = static_cast<double>(mode.hTotal) * mode.vTotal;
                        if (total > 0) {
                            output.refreshHz =
                                static_cast<int>(static_cast<double>(mode.dotClock) / total + 0.5);
                        }
                        break;
                    }

                    found.push_back(std::move(output));
                }
                if (crtc != nullptr) XRRFreeCrtcInfo(crtc);
                XRRFreeOutputInfo(info);
            }
            XRRFreeScreenResources(resources);
        }
    }

    if (found.empty()) {
        // No RandR, or a server that reports no connected output — a VNC or
        // Xvfb display, which is also what CI runs against.
        OutputInfo output;
        output.id = "screen" + std::to_string(screen_);
        output.width = DisplayWidth(display_, screen_);
        output.height = DisplayHeight(display_, screen_);
        output.pixelWidth = output.width;
        output.pixelHeight = output.height;
        found.push_back(std::move(output));
    }

    return found;
}

double X11Backend::visibleFraction(const OutputInfo& output) {
    const Rect bounds = {static_cast<double>(output.x), static_cast<double>(output.y),
                         static_cast<double>(output.x + output.width),
                         static_cast<double>(output.y + output.height)};

    const std::vector<Window> stack =
        windowProperty<Window>(display_, root_, atoms_.clientListStacking, XA_WINDOW);
    if (stack.empty()) {
        // Either no window manager, or one that does not maintain the list. A
        // bare X session with no WM genuinely has nothing covering the desktop;
        // reporting fully visible is both the safe answer and usually the true
        // one.
        return 1.0;
    }

    const std::vector<unsigned long> currentDesktopValue =
        windowProperty<unsigned long>(display_, root_, atoms_.currentDesktop, XA_CARDINAL);
    const unsigned long currentDesktop =
        currentDesktopValue.empty() ? 0 : currentDesktopValue.front();

    std::vector<Rect> occluders;
    occluders.reserve(stack.size());

    for (const Window window : stack) {
        if (std::find(ownWindows_.begin(), ownWindows_.end(), window) != ownWindows_.end()) {
            continue;
        }

        XWindowAttributes attributes = {};
        if (XGetWindowAttributes(display_, window, &attributes) == 0) continue;
        if (attributes.map_state != IsViewable) continue;
        // InputOnly windows are the drag targets and grab helpers toolkits
        // scatter around; they have geometry and cover nothing.
        if (attributes.c_class != InputOutput) continue;

        // The desktop layer and the panels. This is the same judgement the
        // macOS port makes when it counts only layer 0: the menu bar and the
        // Dock are not things users think of as covering their desktop, and
        // counting a panel charged a permanent few percent against the visible
        // budget for furniture that is always there.
        bool skip = false;
        for (const Atom type :
             windowProperty<Atom>(display_, window, atoms_.windowType, XA_ATOM)) {
            if (type == atoms_.windowTypeDesktop || type == atoms_.windowTypeDock ||
                type == atoms_.windowTypeNotification) {
                skip = true;
                break;
            }
        }
        if (skip) continue;

        for (const Atom state : windowProperty<Atom>(display_, window, atoms_.state, XA_ATOM)) {
            if (state == atoms_.stateHidden) {
                skip = true;
                break;
            }
        }
        if (skip) continue;

        // A window on another virtual desktop is not covering this one. 0xFFFF
        // FFFF is EWMH's "on all desktops".
        const std::vector<unsigned long> desktop =
            windowProperty<unsigned long>(display_, window, atoms_.desktop, XA_CARDINAL);
        if (!desktop.empty() && desktop.front() != kAllDesktops &&
            desktop.front() != currentDesktop) {
            continue;
        }

        // A translucent window still shows the wallpaper through it. Treat
        // anything not nearly opaque as not covering: over-estimating what is
        // visible keeps rendering, which is the safe direction to be wrong.
        const std::vector<unsigned long> opacity =
            windowProperty<unsigned long>(display_, window, atoms_.windowOpacity, XA_CARDINAL);
        if (!opacity.empty() &&
            static_cast<double>(opacity.front()) / 0xFFFFFFFFu < 0.95) {
            continue;
        }

        // Coordinates are relative to the parent, and a managed window's parent
        // is the frame the window manager reparented it into. Translating to
        // the root is the only way to get a number that can be compared with an
        // output's position.
        int rootX = 0;
        int rootY = 0;
        Window child = None;
        if (XTranslateCoordinates(display_, window, root_, 0, 0, &rootX, &rootY, &child) == 0) {
            continue;
        }

        // Add the frame the window manager drew around it. Title bars are
        // opaque and cover the desktop just as the client area does.
        double left = rootX;
        double top = rootY;
        double right = rootX + attributes.width;
        double bottom = rootY + attributes.height;

        const std::vector<long> extents =
            windowProperty<long>(display_, window, atoms_.frameExtents, XA_CARDINAL);
        if (extents.size() >= 4) {
            left -= static_cast<double>(extents[0]);
            right += static_cast<double>(extents[1]);
            top -= static_cast<double>(extents[2]);
            bottom += static_cast<double>(extents[3]);
        }

        occluders.push_back({left, top, right, bottom});
    }

    return DesktopVisibility::uncoveredFraction(bounds, occluders);
}

std::optional<bool> X11Backend::displayAsleep() {
    if (!haveDpms_) return std::nullopt;

    CARD16 level = 0;
    BOOL enabled = False;
    if (DPMSInfo(display_, &level, &enabled) != True) return std::nullopt;
    if (enabled != True) return false;
    // Standby, Suspend and Off all mean the panel is dark. Only DPMSModeOn is
    // a display anybody can see.
    return level != DPMSModeOn;
}

std::optional<std::int64_t> X11Backend::idleMillis() {
    if (xssQueryInfo_ == nullptr || xssInfo_ == nullptr) return std::nullopt;
    if (xssQueryInfo_(display_, root_, xssInfo_) == 0) return std::nullopt;
    return static_cast<std::int64_t>(xssInfo_->idle);
}

int X11Backend::eventFd() { return ConnectionNumber(display_); }

bool X11Backend::dispatchEvents() {
    bool outputsChanged = false;

    while (XPending(display_) > 0) {
        XEvent event;
        XNextEvent(display_, &event);

        if (haveRandr_ && event.type >= randrEventBase_ &&
            event.type < randrEventBase_ + RRNumberEvents) {
            // XRRUpdateConfiguration keeps Xlib's cached screen dimensions in
            // step. Without it DisplayWidth() keeps returning the old size for
            // the life of the connection.
            XRRUpdateConfiguration(&event);
            outputsChanged = true;
        }
    }

    return outputsChanged;
}

void X11Backend::flush() {
    if (display_ != nullptr) XFlush(display_);
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------

namespace {

class X11Surface final : public Surface {
public:
    X11Surface(X11Backend& backend, EglDevice& egl) : backend_(backend), egl_(egl) {}

    ~X11Surface() override {
        egl_.destroySurface(eglSurface_);
        if (window_ != None) {
            backend_.unregisterWindow(window_);
            XDestroyWindow(backend_.display(), window_);
        }
        if (colormap_ != None) XFreeColormap(backend_.display(), colormap_);
        XFlush(backend_.display());
    }

    bool create(const OutputInfo& output) {
        Display* display = backend_.display();

        // Match the visual EGL chose, or the window and the surface disagree
        // about depth and eglCreateWindowSurface returns EGL_BAD_MATCH.
        XVisualInfo request = {};
        request.visualid = egl_.nativeVisualId();
        int matches = 0;
        XVisualInfo* visuals = XGetVisualInfo(display, VisualIDMask, &request, &matches);
        if (visuals == nullptr || matches == 0) {
            Log::error("no X visual matches the EGL config");
            if (visuals != nullptr) XFree(visuals);
            return false;
        }

        colormap_ = XCreateColormap(display, backend_.root(), visuals->visual, AllocNone);

        XSetWindowAttributes attributes = {};
        attributes.colormap = colormap_;
        attributes.background_pixel = 0;
        attributes.border_pixel = 0;
        // Unmanaged. The window manager places managed desktop windows itself
        // and most force them to the whole screen, which is wrong the moment
        // there is a second monitor.
        attributes.override_redirect = True;
        attributes.event_mask = ExposureMask | StructureNotifyMask;

        window_ = XCreateWindow(display, backend_.root(), output.x, output.y,
                                static_cast<unsigned>(output.width),
                                static_cast<unsigned>(output.height), 0, visuals->depth,
                                InputOutput, visuals->visual,
                                CWColormap | CWBackPixel | CWBorderPixel | CWOverrideRedirect |
                                    CWEventMask,
                                &attributes);
        XFree(visuals);

        if (window_ == None) {
            Log::error("could not create the wallpaper window");
            return false;
        }

        setWindowProperties(output);
        makeClickThrough();

        XMapWindow(display, window_);
        // Below everything. An override-redirect window is not managed, so
        // nothing else will put it here.
        XLowerWindow(display, window_);
        XFlush(display);

        backend_.registerWindow(window_);

        eglSurface_ = egl_.createWindowSurface(static_cast<std::uintptr_t>(window_));
        if (eglSurface_ == EGL_NO_SURFACE) return false;

        pixelWidth_ = output.pixelWidth;
        pixelHeight_ = output.pixelHeight;
        return true;
    }

    bool makeCurrent() override { return egl_.makeCurrent(eglSurface_); }

    void present() override {
        eglSwapBuffers(egl_.display(), eglSurface_);
    }

    void resize(const OutputInfo& output) override {
        XMoveResizeWindow(backend_.display(), window_, output.x, output.y,
                          static_cast<unsigned>(output.width),
                          static_cast<unsigned>(output.height));
        XLowerWindow(backend_.display(), window_);
        XFlush(backend_.display());
        pixelWidth_ = output.pixelWidth;
        pixelHeight_ = output.pixelHeight;
    }

    int pixelWidth() const override { return pixelWidth_; }
    int pixelHeight() const override { return pixelHeight_; }
    bool isReady() const override { return eglSurface_ != EGL_NO_SURFACE; }

private:
    void setWindowProperties(const OutputInfo& output) {
        Display* display = backend_.display();

        // Set even though the window is unmanaged. A window manager that starts
        // *after* the app — which is what happens when both are in the same
        // autostart batch — reparents what it finds, and these are what tell it
        // to leave this one at the bottom and out of the taskbar.
        const Atom type = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
        XChangeProperty(display, window_, XInternAtom(display, "_NET_WM_WINDOW_TYPE", False),
                        XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&type), 1);

        const Atom states[] = {
            XInternAtom(display, "_NET_WM_STATE_BELOW", False),
            XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False),
            XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False),
            XInternAtom(display, "_NET_WM_STATE_STICKY", False),
        };
        XChangeProperty(display, window_, XInternAtom(display, "_NET_WM_STATE", False), XA_ATOM, 32,
                        PropModeReplace, reinterpret_cast<const unsigned char*>(states),
                        static_cast<int>(sizeof(states) / sizeof(states[0])));

        const unsigned long allDesktops = kAllDesktops;
        XChangeProperty(display, window_, XInternAtom(display, "_NET_WM_DESKTOP", False),
                        XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&allDesktops), 1);

        const std::string title = "LiveWall " + output.id;
        XStoreName(display, window_, title.c_str());

        // WM_CLASS lets a user write a compositor rule against this window —
        // picom's "shadow off for class livewall" being the obvious one.
        XClassHint hint = {};
        char instance[] = "livewall";
        char className[] = "LiveWall";
        hint.res_name = instance;
        hint.res_class = className;
        XSetClassHint(display, window_, &hint);
    }

    void makeClickThrough() {
        Display* display = backend_.display();
        // An empty input region means every event lands on whatever is behind:
        // the desktop icons, or the root window's own menu.
        const XserverRegion region = XFixesCreateRegion(display, nullptr, 0);
        XFixesSetWindowShapeRegion(display, window_, ShapeInput, 0, 0, region);
        XFixesDestroyRegion(display, region);
    }

    X11Backend& backend_;
    EglDevice& egl_;
    Window window_ = None;
    Colormap colormap_ = None;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;
    int pixelWidth_ = 0;
    int pixelHeight_ = 0;
};

}  // namespace

std::unique_ptr<Surface> X11Backend::createSurface(const OutputInfo& output, EglDevice& egl) {
    auto surface = std::make_unique<X11Surface>(*this, egl);
    if (!surface->create(output)) return nullptr;
    return surface;
}

}  // namespace livewall
