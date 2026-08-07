#include "platform/wayland/WaylandBackend.h"

#include <poll.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include "render/EglDevice.h"
#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {
namespace {

// The interface versions asked for. Each is the lowest that carries something
// the app needs, rather than the highest available: binding a version the
// compositor does not implement is a protocol error that kills the connection.
//
//   wl_compositor 4   wl_surface.damage_buffer, so damage is in buffer pixels
//                     rather than surface-local units that scale changes move.
//   wl_output 2       the scale event.
//   layer_shell 1     everything this app uses is in version 1.
//   xdg_output 2      the name event, which is the only place a connector name
//                     ("DP-1") appears in the Wayland protocol at all.
constexpr std::uint32_t kCompositorVersion = 4;
constexpr std::uint32_t kOutputVersion = 2;
constexpr std::uint32_t kLayerShellVersion = 1;
constexpr std::uint32_t kXdgOutputVersion = 2;

WaylandBackend::OutputRecord* recordFor(WaylandBackend* backend, wl_output* output) {
    for (WaylandBackend::OutputRecord& record : backend->outputRecords()) {
        if (record.output == output) return &record;
    }
    return nullptr;
}

// --- wl_output --------------------------------------------------------------

void outputGeometry(void* data, wl_output* output, int32_t x, int32_t y, int32_t physicalWidth,
                    int32_t physicalHeight, int32_t subpixel, const char* make, const char* model,
                    int32_t transform) {
    // Physical size and make/model are not used: xdg_output supplies the
    // logical geometry, which is what window coordinates are expressed in, and
    // the connector name is a better identifier than a monitor's EDID string.
}

void outputMode(void* data, wl_output* output, uint32_t flags, int32_t width, int32_t height,
                int32_t refresh) {
    auto* backend = static_cast<WaylandBackend*>(data);
    WaylandBackend::OutputRecord* record = recordFor(backend, output);
    if (record == nullptr) return;
    // A display advertises every mode it supports; only the current one is the
    // one being driven.
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) return;

    record->modeWidth = width;
    record->modeHeight = height;
    // Refresh arrives in millihertz.
    record->refreshHz = refresh > 0 ? (refresh + 500) / 1000 : 0;
}

void outputScale(void* data, wl_output* output, int32_t factor) {
    auto* backend = static_cast<WaylandBackend*>(data);
    if (WaylandBackend::OutputRecord* record = recordFor(backend, output); record != nullptr) {
        record->scale = factor > 0 ? factor : 1;
    }
}

void outputDone(void* data, wl_output* output) {
    auto* backend = static_cast<WaylandBackend*>(data);
    if (WaylandBackend::OutputRecord* record = recordFor(backend, output); record != nullptr) {
        record->complete = true;
        backend->markOutputsChanged();
    }
}

// `name` and `description` arrived with wl_output version 4, which shipped in
// libwayland 1.20. Older libwayland — 1.18 on Ubuntu 20.04, which is still a
// supported LTS — declares a four-member listener, and initialising six there
// is a hard error rather than a warning:
//
//     error: too many initializers for 'const wl_output_listener'
//
// wayland-client-protocol.h defines this macro only when the events exist, so
// it is the version test that cannot drift from the header actually in use.
// Nothing is lost when they are absent: both handlers are already no-ops,
// because xdg_output.name carries the same string at xdg_output version 2,
// which is what this app binds.
#ifdef WL_OUTPUT_NAME_SINCE_VERSION

void outputName(void* data, wl_output* output, const char* name) {}

void outputDescription(void* data, wl_output* output, const char* description) {}

#endif

// Listed in the order the struct declares them — geometry, mode, done, scale —
// which is not the order the protocol documents them in.
const wl_output_listener kOutputListener = {
    outputGeometry,
    outputMode,
    outputDone,
    outputScale,
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
    outputName,
    outputDescription,
#endif
};

// --- xdg_output -------------------------------------------------------------

WaylandBackend::OutputRecord* recordForXdg(WaylandBackend* backend, zxdg_output_v1* xdgOutput) {
    for (WaylandBackend::OutputRecord& record : backend->outputRecords()) {
        if (record.xdgOutput == xdgOutput) return &record;
    }
    return nullptr;
}

void xdgOutputLogicalPosition(void* data, zxdg_output_v1* output, int32_t x, int32_t y) {
    auto* backend = static_cast<WaylandBackend*>(data);
    if (auto* record = recordForXdg(backend, output); record != nullptr) {
        record->logicalX = x;
        record->logicalY = y;
    }
}

void xdgOutputLogicalSize(void* data, zxdg_output_v1* output, int32_t width, int32_t height) {
    auto* backend = static_cast<WaylandBackend*>(data);
    if (auto* record = recordForXdg(backend, output); record != nullptr) {
        record->logicalWidth = width;
        record->logicalHeight = height;
    }
}

void xdgOutputDone(void* data, zxdg_output_v1* output) {
    static_cast<WaylandBackend*>(data)->markOutputsChanged();
}

void xdgOutputName(void* data, zxdg_output_v1* output, const char* name) {
    auto* backend = static_cast<WaylandBackend*>(data);
    if (auto* record = recordForXdg(backend, output); record != nullptr) {
        record->connector = name != nullptr ? name : "";
    }
}

void xdgOutputDescription(void* data, zxdg_output_v1* output, const char* description) {}

const zxdg_output_v1_listener kXdgOutputListener = {
    xdgOutputLogicalPosition, xdgOutputLogicalSize, xdgOutputDone, xdgOutputName,
    xdgOutputDescription,
};

// --- registry ---------------------------------------------------------------

void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface,
                    uint32_t version) {
    auto* backend = static_cast<WaylandBackend*>(data);
    WaylandBackend::Globals& globals = backend->globals();

    auto bind = [&](const wl_interface* type, std::uint32_t wanted) {
        return wl_registry_bind(registry, name, type, std::min(version, wanted));
    };

    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        globals.compositor =
            static_cast<wl_compositor*>(bind(&wl_compositor_interface, kCompositorVersion));
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        globals.layerShell = static_cast<zwlr_layer_shell_v1*>(
            bind(&zwlr_layer_shell_v1_interface, kLayerShellVersion));
    } else if (std::strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        globals.xdgOutputManager = static_cast<zxdg_output_manager_v1*>(
            bind(&zxdg_output_manager_v1_interface, kXdgOutputVersion));
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        WaylandBackend::OutputRecord record;
        record.registryName = name;
        record.output = static_cast<wl_output*>(bind(&wl_output_interface, kOutputVersion));
        backend->outputRecords().push_back(record);
        wl_output_add_listener(backend->outputRecords().back().output, &kOutputListener, backend);
        backend->markOutputsChanged();
    }
}

void registryGlobalRemove(void* data, wl_registry* registry, uint32_t name) {
    auto* backend = static_cast<WaylandBackend*>(data);
    std::vector<WaylandBackend::OutputRecord>& records = backend->outputRecords();

    const auto hit = std::find_if(records.begin(), records.end(),
                                  [name](const WaylandBackend::OutputRecord& record) {
                                      return record.registryName == name;
                                  });
    if (hit == records.end()) return;

    if (hit->xdgOutput != nullptr) zxdg_output_v1_destroy(hit->xdgOutput);
    if (hit->output != nullptr) wl_output_destroy(hit->output);
    records.erase(hit);
    backend->markOutputsChanged();
}

const wl_registry_listener kRegistryListener = {registryGlobal, registryGlobalRemove};

}  // namespace

std::unique_ptr<WaylandBackend> WaylandBackend::open() {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr && std::getenv("WAYLAND_SOCKET") == nullptr) {
        return nullptr;
    }

    std::unique_ptr<WaylandBackend> backend(new WaylandBackend());
    if (!backend->initialize()) return nullptr;
    return backend;
}

bool WaylandBackend::initialize() {
    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) {
        Log::info("no Wayland display");
        return false;
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &kRegistryListener, this);

    // Two round trips, not one. The first delivers the registry's globals; the
    // second delivers the events those globals then emit — an output's mode and
    // scale arrive only after that output has been bound, which happens during
    // the first.
    wl_display_roundtrip(display_);
    wl_display_roundtrip(display_);

    if (globals_.compositor == nullptr) {
        Log::error("the Wayland compositor offers no wl_compositor");
        return false;
    }

    if (globals_.layerShell == nullptr) {
        // The common case is GNOME. Not an error: Backend::create takes this as
        // the signal to use X11 through XWayland, which works.
        Log::info("this compositor has no wlr-layer-shell — nowhere to put a background surface");
        return false;
    }

    if (globals_.xdgOutputManager != nullptr) {
        for (OutputRecord& record : outputs_) {
            record.xdgOutput =
                zxdg_output_manager_v1_get_xdg_output(globals_.xdgOutputManager, record.output);
            zxdg_output_v1_add_listener(record.xdgOutput, &kXdgOutputListener, this);
        }
        wl_display_roundtrip(display_);
    } else {
        Log::info("no xdg-output — outputs will be identified by index, not connector name");
    }

    return true;
}

WaylandBackend::~WaylandBackend() {
    for (OutputRecord& record : outputs_) {
        if (record.xdgOutput != nullptr) zxdg_output_v1_destroy(record.xdgOutput);
        if (record.output != nullptr) wl_output_destroy(record.output);
    }
    if (globals_.layerShell != nullptr) {
        // Not zwlr_layer_shell_v1_destroy(): that sends the protocol's own
        // `destroy` request, which only exists from version 3, and this object
        // is bound at version 1. Sending a request the compositor does not know
        // about is a protocol error that kills the connection — during
        // shutdown, where it would look like a crash. Destroying the proxy
        // locally is what a version-1 client is supposed to do.
        wl_proxy_destroy(reinterpret_cast<wl_proxy*>(globals_.layerShell));
    }
    if (globals_.xdgOutputManager != nullptr) {
        zxdg_output_manager_v1_destroy(globals_.xdgOutputManager);
    }
    if (globals_.compositor != nullptr) wl_compositor_destroy(globals_.compositor);
    if (registry_ != nullptr) wl_registry_destroy(registry_);
    if (display_ != nullptr) wl_display_disconnect(display_);
}

unsigned WaylandBackend::eglPlatform() const { return EGL_PLATFORM_WAYLAND_KHR; }

void* WaylandBackend::nativeDisplay() const { return display_; }

std::vector<OutputInfo> WaylandBackend::outputs() {
    std::vector<OutputInfo> found;
    int index = 0;

    for (const OutputRecord& record : outputs_) {
        if (!record.complete) continue;

        OutputInfo output;
        output.id = record.connector.empty() ? ("output" + std::to_string(index)) : record.connector;
        output.x = record.logicalX;
        output.y = record.logicalY;

        // Without xdg-output there is no logical size, only the mode. Dividing
        // the mode by the scale is what the compositor itself would have
        // reported, and is right for every integer scale — which is all
        // wl_output can express.
        output.width = record.logicalWidth > 0 ? record.logicalWidth : record.modeWidth / record.scale;
        output.height =
            record.logicalHeight > 0 ? record.logicalHeight : record.modeHeight / record.scale;

        output.scale = record.scale;
        output.pixelWidth = output.width * record.scale;
        output.pixelHeight = output.height * record.scale;
        output.refreshHz = record.refreshHz;

        if (output.width > 0 && output.height > 0) found.push_back(std::move(output));
        ++index;
    }

    return found;
}

int WaylandBackend::eventFd() {
    return display_ != nullptr ? wl_display_get_fd(display_) : -1;
}

bool WaylandBackend::dispatchEvents() {
    outputsChanged_ = false;
    if (display_ == nullptr) return false;

    // The prepare/read dance rather than wl_display_dispatch(). The plain call
    // blocks when the queue is empty and the socket has nothing on it, which
    // would park the whole app — the control socket, the bus and every output's
    // pump are on this same loop. This version reads only what is already
    // there.
    wl_display_dispatch_pending(display_);

    while (wl_display_prepare_read(display_) != 0) {
        wl_display_dispatch_pending(display_);
    }
    wl_display_flush(display_);

    pollfd descriptor = {};
    descriptor.fd = wl_display_get_fd(display_);
    descriptor.events = POLLIN;
    if (::poll(&descriptor, 1, 0) > 0 && (descriptor.revents & POLLIN) != 0) {
        wl_display_read_events(display_);
        wl_display_dispatch_pending(display_);
    } else {
        wl_display_cancel_read(display_);
    }

    return outputsChanged_;
}

void WaylandBackend::flush() {
    if (display_ != nullptr) wl_display_flush(display_);
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------

namespace {

class WaylandSurface final : public Surface {
public:
    WaylandSurface(WaylandBackend& backend, EglDevice& egl) : backend_(backend), egl_(egl) {}

    ~WaylandSurface() override {
        egl_.destroySurface(eglSurface_);
        if (frameCallback_ != nullptr) wl_callback_destroy(frameCallback_);
        if (eglWindow_ != nullptr) wl_egl_window_destroy(eglWindow_);
        if (layerSurface_ != nullptr) zwlr_layer_surface_v1_destroy(layerSurface_);
        if (surface_ != nullptr) wl_surface_destroy(surface_);
        wl_display_flush(backend_.display());
    }

    bool create(const OutputInfo& info, wl_output* output) {
        WaylandBackend::Globals& globals = backend_.globals();

        surface_ = wl_compositor_create_surface(globals.compositor);
        if (surface_ == nullptr) return false;

        // An empty input region. The protocol has a field for this, which is
        // the one place the Wayland path is simply nicer than shaping an X11
        // window with XFixes.
        if (wl_region* empty = wl_compositor_create_region(globals.compositor); empty != nullptr) {
            wl_surface_set_input_region(surface_, empty);
            wl_region_destroy(empty);
        }

        layerSurface_ = zwlr_layer_shell_v1_get_layer_surface(
            globals.layerShell, surface_, output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
        if (layerSurface_ == nullptr) return false;

        zwlr_layer_surface_v1_add_listener(layerSurface_, &kLayerListener, this);

        // Anchored to all four edges with a size of 0x0 means "the whole
        // output, and tell me how big that is" — the configure event carries
        // the answer, which is why the surface is not ready until it arrives.
        zwlr_layer_surface_v1_set_anchor(layerSurface_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(layerSurface_, 0, 0);
        // -1 rather than 0: a zero exclusive zone means "respect other
        // surfaces' exclusive zones", which shrinks the wallpaper by the height
        // of every panel on the output and leaves a strip of nothing behind
        // them.
        zwlr_layer_surface_v1_set_exclusive_zone(layerSurface_, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            layerSurface_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

        wl_surface_commit(surface_);
        // The configure arrives during this round trip; without it `create`
        // would return a surface nothing may draw on yet.
        wl_display_roundtrip(backend_.display());

        if (!configured_) {
            Log::error("the compositor never configured the layer surface for " + info.id);
            return false;
        }

        scale_ = static_cast<int>(info.scale > 0 ? info.scale : 1);
        return buildEglWindow();
    }

    bool makeCurrent() override {
        return eglSurface_ != EGL_NO_SURFACE && egl_.makeCurrent(eglSurface_);
    }

    void present() override {
        // Requested before the swap, so the compositor sees the request and the
        // buffer in the same commit. Asking afterwards races: eglSwapBuffers
        // commits, and a callback added after that belongs to the *next* frame.
        requestFrameCallback();
        frameReady_ = false;
        eglSwapBuffers(egl_.display(), eglSurface_);
    }

    void resize(const OutputInfo& output) override {
        scale_ = static_cast<int>(output.scale > 0 ? output.scale : 1);
        applySize(output.width, output.height);
    }

    int pixelWidth() const override { return width_ * scale_; }
    int pixelHeight() const override { return height_ * scale_; }
    bool isReady() const override { return configured_ && eglSurface_ != EGL_NO_SURFACE; }
    bool readyForFrame() const override { return frameReady_; }

private:
    static void layerConfigure(void* data, zwlr_layer_surface_v1* layerSurface, uint32_t serial,
                               uint32_t width, uint32_t height) {
        auto* self = static_cast<WaylandSurface*>(data);
        // Acknowledging before resizing is required: the compositor treats the
        // next commit as the response to this serial.
        zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
        self->configured_ = true;
        self->applySize(static_cast<int>(width), static_cast<int>(height));
    }

    static void layerClosed(void* data, zwlr_layer_surface_v1* layerSurface) {
        auto* self = static_cast<WaylandSurface*>(data);
        // The output went away, or the compositor is shutting down. The engine
        // notices through the registry's global_remove and drops this surface;
        // marking it unready stops the pump in the meantime.
        self->configured_ = false;
        self->backend_.markOutputsChanged();
    }

    static void frameDone(void* data, wl_callback* callback, uint32_t time) {
        auto* self = static_cast<WaylandSurface*>(data);
        wl_callback_destroy(callback);
        self->frameCallback_ = nullptr;
        self->frameReady_ = true;
    }

    void requestFrameCallback() {
        if (frameCallback_ != nullptr) return;
        frameCallback_ = wl_surface_frame(surface_);
        wl_callback_add_listener(frameCallback_, &kFrameListener, this);
    }

    void applySize(int width, int height) {
        if (width <= 0 || height <= 0) return;
        if (width == width_ && height == height_) return;

        width_ = width;
        height_ = height;
        if (eglWindow_ != nullptr) {
            wl_egl_window_resize(eglWindow_, pixelWidth(), pixelHeight(), 0, 0);
            wl_surface_set_buffer_scale(surface_, scale_);
        }
    }

    bool buildEglWindow() {
        eglWindow_ = wl_egl_window_create(surface_, pixelWidth(), pixelHeight());
        if (eglWindow_ == nullptr) {
            Log::error("wl_egl_window_create failed");
            return false;
        }
        wl_surface_set_buffer_scale(surface_, scale_);

        eglSurface_ = egl_.createWindowSurface(reinterpret_cast<std::uintptr_t>(eglWindow_));
        if (eglSurface_ == EGL_NO_SURFACE) return false;

        // The first frame is unconditional; after that the compositor's
        // callbacks pace it.
        frameReady_ = true;
        return true;
    }

    static const zwlr_layer_surface_v1_listener kLayerListener;
    static const wl_callback_listener kFrameListener;

    WaylandBackend& backend_;
    EglDevice& egl_;
    wl_surface* surface_ = nullptr;
    zwlr_layer_surface_v1* layerSurface_ = nullptr;
    wl_egl_window* eglWindow_ = nullptr;
    wl_callback* frameCallback_ = nullptr;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;
    int width_ = 0;
    int height_ = 0;
    int scale_ = 1;
    bool configured_ = false;
    bool frameReady_ = false;
};

const zwlr_layer_surface_v1_listener WaylandSurface::kLayerListener = {
    WaylandSurface::layerConfigure,
    WaylandSurface::layerClosed,
};

const wl_callback_listener WaylandSurface::kFrameListener = {WaylandSurface::frameDone};

}  // namespace

std::unique_ptr<Surface> WaylandBackend::createSurface(const OutputInfo& output, EglDevice& egl) {
    wl_output* target = nullptr;
    for (const OutputRecord& record : outputs_) {
        if (record.connector == output.id) {
            target = record.output;
            break;
        }
    }
    // A null wl_output is legal and means "let the compositor choose", which is
    // the right fallback on a compositor without xdg-output where the id was
    // synthesised from an index.
    auto surface = std::make_unique<WaylandSurface>(*this, egl);
    if (!surface->create(output, target)) return nullptr;
    return surface;
}

}  // namespace livewall
