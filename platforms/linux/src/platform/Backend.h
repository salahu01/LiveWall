// The display server, behind one interface.
//
// Two implementations, and they are not equals. X11 can answer the question
// this whole app is built around — is anything covering the desktop right now —
// and Wayland cannot. Everything else the interface exposes is symmetric; that
// one method is the reason the app behaves differently on the two, and
// `supportsOcclusion()` is what the rest of the code branches on rather than
// checking which backend it got.
//
// The split of responsibilities with the render layer: the backend owns the
// connection, the outputs and the native surfaces; `EglDevice` owns the EGL
// display, config and context and knows nothing about either display server
// beyond the two handles it is given.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace livewall {

class EglDevice;

// One display, as the app needs to think about it.
struct OutputInfo {
    // Stable across a disconnect and reconnect: the connector name ("DP-1",
    // "eDP-1"). Used as the key for per-output state and printed by
    // `livewall status`.
    std::string id;

    // Logical position and size, in the coordinate space window geometry is
    // reported in. On X11 that is pixels; on Wayland it is the compositor's
    // logical space, which is pixels divided by the scale factor.
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    // The buffer size — what actually has to be rendered, and what the
    // transcoder sizes against.
    int pixelWidth = 0;
    int pixelHeight = 0;

    double scale = 1.0;

    // Used to pick a frame rate that divides the refresh exactly. 0 when the
    // server does not report one, which the pacer treats as 60.
    int refreshHz = 0;

    bool operator==(const OutputInfo& other) const {
        return id == other.id && x == other.x && y == other.y && width == other.width &&
               height == other.height && pixelWidth == other.pixelWidth &&
               pixelHeight == other.pixelHeight && refreshHz == other.refreshHz;
    }
};

// One output's drawable. Owns a native window and the EGL surface on it.
class Surface {
public:
    virtual ~Surface() = default;

    virtual bool makeCurrent() = 0;
    virtual void present() = 0;

    // Called when the output's geometry changed. Cheaper than destroying the
    // surface, and on Wayland it is the only correct response to a configure.
    virtual void resize(const OutputInfo& output) = 0;

    virtual int pixelWidth() const = 0;
    virtual int pixelHeight() const = 0;

    // True once the compositor has acknowledged the surface and it is safe to
    // draw. Always true on X11; on Wayland the first configure has to arrive
    // first, and drawing before it is a protocol error.
    virtual bool isReady() const = 0;

    // Whether the display server wants another frame right now.
    //
    // Always true on X11, where nothing asks. On Wayland it is the frame
    // callback, and it is the closest thing that backend has to an occlusion
    // signal: a compositor that knows this surface is completely hidden stops
    // sending callbacks, so the pump stalls on its own. It is a weaker signal
    // than the X11 window walk in two ways — it is all-or-nothing rather than a
    // fraction, and a compositor is free to keep sending callbacks for a hidden
    // surface — but it costs nothing and it is what there is.
    virtual bool readyForFrame() const { return true; }
};

class Backend {
public:
    virtual ~Backend() = default;

    // `preference` is "auto", "x11" or "wayland". Auto picks Wayland only when
    // WAYLAND_DISPLAY is set *and* the compositor offers wlr-layer-shell —
    // without that protocol there is nowhere to put a background surface, and
    // falling through to X11 via XWayland is both possible and better than
    // failing.
    static std::unique_ptr<Backend> create(std::string_view preference);

    virtual const char* name() const = 0;

    // False on Wayland. See DesktopVisibility.h for why this is not a gap that
    // can be closed with more work.
    virtual bool supportsOcclusion() const = 0;

    // Whether an EGL config with an alpha channel is worth asking for.
    //
    // Always true on Wayland — the compositor is the compositor. On X11 it
    // depends on whether a compositing manager is running: an ARGB visual with
    // nothing to composite it renders as opaque black, which is exactly what
    // the Fit letterbox must not be.
    //
    // Asked through the interface rather than by downcasting so that no file
    // above this layer includes Xlib. That header defines `None`, `Status` and
    // `Success` as macros, and they collide with ordinary identifiers in
    // unrelated code — which is how this method came to exist.
    virtual bool prefersAlpha() const = 0;

    virtual std::vector<OutputInfo> outputs() = 0;

    virtual std::unique_ptr<Surface> createSurface(const OutputInfo& output, EglDevice& egl) = 0;

    // Fraction of this output's desktop not covered by an opaque window, 0...1.
    // Returns 1 when it cannot be measured, so a failure here can never be what
    // stops a wallpaper.
    virtual double visibleFraction(const OutputInfo& output) = 0;

    // Nothing when this backend cannot tell, which is different from "no".
    virtual std::optional<bool> displayAsleep() = 0;
    virtual std::optional<std::int64_t> idleMillis() = 0;

    // Polled by the one event loop. -1 when the backend has no descriptor.
    virtual int eventFd() = 0;

    // Drains pending events. Returns true when the output set changed and the
    // engine should re-sync.
    virtual bool dispatchEvents() = 0;

    // Anything queued that has not reached the server yet. Wayland needs this
    // before the loop blocks; on X11 it is XFlush.
    virtual void flush() = 0;

    // For EglDevice. `platform` is an EGL_PLATFORM_* enum, `display` the
    // native handle (Display* or wl_display*).
    virtual unsigned eglPlatform() const = 0;
    virtual void* nativeDisplay() const = 0;
};

}  // namespace livewall
