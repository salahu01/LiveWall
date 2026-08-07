// How much of an output's desktop is actually left uncovered.
//
// This file is the geometry only. Where the rectangles come from is a property
// of the display server and lives in the backend, and the difference between
// the two backends is the single largest difference between them:
//
//   X11     The window list is public. `_NET_CLIENT_LIST_STACKING` on the root
//           window names every managed top-level, and each one's geometry,
//           type, state and opacity can be read. So the same measurement the
//           macOS port derives from CGWindowListCopyWindowInfo is available,
//           and the wallpaper stops when a browser is maximised over it.
//
//   Wayland There is no window list, by design. A client cannot learn that
//           another client's surface exists, let alone where it is. Nothing in
//           wlr-layer-shell, xdg-shell or any staging protocol exposes it, and
//           the security model is the reason rather than an oversight. The
//           backend reports 1.0 — fully visible — and the wallpaper leans on
//           frame callbacks instead: a compositor that knows the background is
//           hidden simply stops sending them, which stops the pump but does not
//           tear the decoder down.
//
// That asymmetry is why the X11 backend is the one this port is built around.
// It is written up in the README under "What Wayland costs".
//
// On X11, only window *geometry* is read — bounds, type, state and opacity.
// Nothing here reads a title or a pixel, so it needs no capture permission and
// no compositor extension.
#pragma once

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
    // Fraction of `bounds` not covered by an opaque window, 0...1. Rectangles
    // may overlap and may hang off the edge of `bounds`; both are normal.
    //
    // Coverage is measured by marking cells on a grid rather than computing an
    // exact rectangle union. The answer feeds a threshold comparison, so ~1%
    // precision is ample and this stays a few hundred integer operations
    // against code that would otherwise need a full sweep-line.
    //
    // A cell counts as covered when its centre is inside an occluder, so a
    // window edge cutting through a cell rounds to the side it covers most.
    static double uncoveredFraction(const Rect& bounds, const std::vector<Rect>& occluders);

    static constexpr int kGridColumns = 64;
    static constexpr int kGridRows = 40;
};

}  // namespace livewall
