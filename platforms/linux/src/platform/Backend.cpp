#include "platform/Backend.h"

#include <cstdlib>

#include "platform/x11/X11Backend.h"
#include "support/Log.h"

#if LIVEWALL_HAS_WAYLAND
#include "platform/wayland/WaylandBackend.h"
#endif

namespace livewall {

std::unique_ptr<Backend> Backend::create(std::string_view preference) {
    const bool wantX11 = preference == "x11";
    const bool wantWayland = preference == "wayland";

#if LIVEWALL_HAS_WAYLAND
    // Auto order: X11 first.
    //
    // That looks backwards on a machine running a Wayland compositor, and it is
    // deliberate. The feature this app exists for is tearing the decoder down
    // when nothing can see the wallpaper, and only the X11 backend can tell.
    // Under a Wayland session with XWayland — which is nearly all of them — the
    // X11 backend still works, and it still cannot see native Wayland windows,
    // so the occlusion it reports is partial. Partial is more than none.
    //
    // The one case where that reasoning inverts is a compositor with no
    // XWayland at all, where X11 simply fails to open and this falls through.
    if (wantWayland || (!wantX11 && std::getenv("DISPLAY") == nullptr)) {
        if (auto wayland = WaylandBackend::open()) return wayland;
        if (wantWayland) {
            Log::error("no Wayland backend available (needs wlr-layer-shell)");
            return nullptr;
        }
    }
#else
    if (wantWayland) {
        Log::error("this build has no Wayland backend");
        return nullptr;
    }
#endif

    if (auto x11 = X11Backend::open()) return x11;

    if (wantX11) {
        Log::error("no X display");
        return nullptr;
    }

#if LIVEWALL_HAS_WAYLAND
    // X11 was preferred and is not there. Wayland is the remaining option, and
    // if it has no layer shell either then there is genuinely nowhere to draw.
    if (auto wayland = WaylandBackend::open()) return wayland;
#endif

    Log::error("no display server — set DISPLAY or WAYLAND_DISPLAY");
    return nullptr;
}

}  // namespace livewall
