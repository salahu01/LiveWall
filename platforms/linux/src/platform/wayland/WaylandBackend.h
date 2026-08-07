// The Wayland backend, on wlr-layer-shell.
//
// What it can do: put a surface on the background layer of every output, below
// every application window and above nothing, sized by the compositor, taking
// no input. That is a better fit for the job than anything X11 offers — there
// is no stacking to fight, no window manager to second-guess the geometry, and
// no need to shape an input region because the protocol has a field for it.
//
// What it cannot do: tell whether anything is covering it. See
// DesktopVisibility.h. The consequence is concrete — a maximised browser on X11
// tears the decoder down, and on Wayland it does not unless the compositor
// stops sending frame callbacks — and it is why `Backend::create` prefers X11
// when a session offers both.
//
// Compositor support is the other half of that. wlr-layer-shell is implemented
// by wlroots (sway, Hyprland, river, Wayfire), KDE's KWin and a few others, and
// is *not* implemented by GNOME's Mutter. On GNOME under Wayland this backend
// finds no layer shell and `Backend::create` falls through to X11, which works
// through XWayland — with the same blindness to occlusion, since XWayland
// clients cannot see native Wayland windows either.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "platform/Backend.h"

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_output;
struct zwlr_layer_shell_v1;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;

namespace livewall {

class WaylandBackend final : public Backend {
public:
    // Returns null when WAYLAND_DISPLAY is unset, the socket will not connect,
    // or the compositor does not offer wlr-layer-shell. None of those is an
    // error — they are how `Backend::create` decides to use X11 instead.
    static std::unique_ptr<WaylandBackend> open();

    ~WaylandBackend() override;

    const char* name() const override { return "wayland"; }
    bool supportsOcclusion() const override { return false; }
    bool prefersAlpha() const override { return true; }

    std::vector<OutputInfo> outputs() override;
    std::unique_ptr<Surface> createSurface(const OutputInfo& output, EglDevice& egl) override;

    // Always 1.0. A Wayland client cannot see other clients' surfaces.
    double visibleFraction(const OutputInfo& output) override { return 1.0; }

    // Neither is knowable here. Nothing in the core protocol reports DPMS, and
    // ext-idle-notify tells a client when it *becomes* idle rather than how
    // long it has been — so PowerMonitor falls back to logind's IdleHint, which
    // is what the compositor told logind anyway.
    std::optional<bool> displayAsleep() override { return std::nullopt; }
    std::optional<std::int64_t> idleMillis() override { return std::nullopt; }

    int eventFd() override;
    bool dispatchEvents() override;
    void flush() override;

    unsigned eglPlatform() const override;
    void* nativeDisplay() const override;

    // Per-output state the registry listener fills in. Public because the
    // C-ABI listener callbacks are free functions in the .cpp.
    struct OutputRecord {
        wl_output* output = nullptr;
        zxdg_output_v1* xdgOutput = nullptr;
        std::uint32_t registryName = 0;

        std::string connector;   // "DP-1", from xdg_output.name
        int logicalX = 0;
        int logicalY = 0;
        int logicalWidth = 0;
        int logicalHeight = 0;
        int modeWidth = 0;
        int modeHeight = 0;
        int refreshHz = 0;
        int scale = 1;
        bool complete = false;
    };

    struct Globals {
        wl_compositor* compositor = nullptr;
        zwlr_layer_shell_v1* layerShell = nullptr;
        zxdg_output_manager_v1* xdgOutputManager = nullptr;
    };

    Globals& globals() { return globals_; }
    std::vector<OutputRecord>& outputRecords() { return outputs_; }
    void markOutputsChanged() { outputsChanged_ = true; }
    wl_display* display() const { return display_; }

private:
    WaylandBackend() = default;
    bool initialize();

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    Globals globals_;
    std::vector<OutputRecord> outputs_;
    bool outputsChanged_ = false;
};

}  // namespace livewall
