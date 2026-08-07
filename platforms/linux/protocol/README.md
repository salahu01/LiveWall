# Vendored Wayland protocol definitions

Three XML files, unmodified from upstream, turned into C by `wayland-scanner`
at build time. They are checked in rather than read out of
`/usr/share/wayland-protocols` so the build does not change shape depending on
which release a distribution happens to carry — and because one of them is not
in `wayland-protocols` at all.

| File | Upstream | Licence |
|---|---|---|
| `wlr-layer-shell-unstable-v1.xml` | [wlroots/wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols) | MIT-style, © 2017 Drew DeVault |
| `xdg-shell.xml` | wayland-protocols, `stable/xdg-shell` | MIT |
| `xdg-output-unstable-v1.xml` | wayland-protocols, `unstable/xdg-output` | MIT |

Each file carries its own `<copyright>` block; none has been edited.

`wlr-layer-shell` is the only way for a client to put a surface *behind* every
other window. It ships with wlroots, which LiveWall does not otherwise need or
link — so vendoring the 19 KB definition is cheaper than depending on wlroots
being installed.

`xdg-shell` is here because `zwlr_layer_surface_v1` inherits configure/ack
semantics that `wayland-scanner` will not generate a header for on its own.
`xdg-output` supplies each output's logical position and size, which plain
`wl_output` reports only in physical pixels.
