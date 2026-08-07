<img src="../../docs/icon.png" width="128" align="right" alt="LiveWall icon">

# LiveWall for Linux

A live wallpaper daemon built around one constraint: **it should cost almost
nothing when you aren't looking at it.**

The same design exists for [macOS](../macos/README.md) and
[Windows](../windows/README.md). This port keeps every decision those make and
changes three, because Linux forces it to: there is no guaranteed media
framework, there is no single way to put something behind the desktop, and
under Wayland there is no way to find out whether anything is covering it.

Each of those is written up below rather than papered over.

```
livewall add ~/Videos/aurora.mp4     # import and play it
livewall status                      # what it is doing and why
livewall stop                        # back to the procedural gradient
```

## The dependency answer

macOS has AVFoundation and Windows has Media Foundation. Linux has neither, so
the question becomes *when* rather than *whether* — and the answer is that
almost nothing is a build-time dependency of the binary that ships.

| | Libraries | If absent |
|---|---|---|
| **Linked** | libX11, libXrandr, libXext, libXfixes, libEGL, libGLESv2, libwayland-client, libwayland-egl | A desktop Linux machine has these or it has no desktop. |
| **dlopen'd** | libavformat, libavcodec, libavutil, libswscale | No video wallpapers. The procedural mode still runs. |
| | libva (through FFmpeg) | Software decode instead of zero-copy. Costs about an order of magnitude more CPU. |
| | libdbus-1 | No tray icon, and the lock / idle-hint / power-profile gates are off. Every other gate still applies. |
| | libXss | No idle detection on X11. logind's `IdleHint` stands in where it exists. |

`livewall probe` prints which of those this machine has. The binary is
**459 KB** and `ldd` on it lists only the linked row — checked in CI, so an
accidental `-l` cannot quietly turn an optional dependency into a required one.

## What Wayland costs

This port has two backends and they are not equals.

The feature the whole project exists for is that the decoder is **destroyed**
when nothing can see the wallpaper. Deciding that needs an answer to "is
anything covering this output right now", and:

- **X11 can answer it.** `_NET_CLIENT_LIST_STACKING` on the root window names
  every managed top-level, and each one's geometry, type, state and opacity can
  be read. So the same measurement the macOS port derives from
  `CGWindowListCopyWindowInfo` is available here, and a maximised browser really
  does tear the decoder down.

- **Wayland cannot, by design.** No client can learn that another client's
  surface exists, let alone where it is. Nothing in wlr-layer-shell, xdg-shell
  or any staging protocol exposes it, and the security model is the reason
  rather than an oversight.

What the Wayland backend has instead is the frame callback: a compositor that
knows the background surface is completely hidden stops asking for frames, and
the pump stalls. That is weaker in two specific ways — it is all-or-nothing
rather than a fraction, so the "desktop 95% covered" case keeps decoding, and a
compositor is free to keep sending callbacks for a hidden surface.

So `Backend::create` prefers **X11 first**, even on a Wayland session. Under
XWayland the X11 backend still works, and it still cannot see native Wayland
windows — but partial occlusion information is more than none. Set
`backend: "wayland"` in `~/.config/livewall/settings.json` to override.

`livewall status` says which one is live and whether it can see anything:

```
backend     x11 (occlusion aware)
backend     wayland (cannot see other windows)
```

### Compositor support

wlr-layer-shell is implemented by wlroots (sway, Hyprland, river, Wayfire) and
by KWin. It is **not** implemented by GNOME's Mutter. On GNOME under Wayland
this backend finds no layer shell and the app falls through to X11 via
XWayland, which works.

## Measured

**No hardware numbers yet, and inventing them would defeat the point of a
README that quotes them.** This port has been developed and verified in a
container with software rendering (llvmpipe under Xvfb, pixman under headless
sway), where the memory figure is dominated by Mesa's software rasteriser and
tells you nothing about a real GPU.

What *is* verified:

| | |
|---|---|
| Binary size | **459 KB** (macOS: 504 KB) |
| Links only the guaranteed set | yes — `ldd` checked in CI |
| Builds and passes its tests | yes — 10 suites, no display needed |
| Renders on X11 | yes, under Xvfb |
| Renders on Wayland | yes, under headless sway with wlr-layer-shell |
| Full import → index → playback | yes, HEVC via libx265 |

Reproduce the cost figures on your own machine with
[`tools/measure.sh`](tools/measure.sh), which samples Pss from
`/proc/<pid>/smaps_rollup` and CPU from `/proc/<pid>/stat` — the same figure
`livewall status` reports, and the closest Linux equivalent to the
`phys_footprint` the macOS README quotes.

To measure the *playback* cost on a machine whose desktop is behind a terminal,
start the daemon with `LIVEWALL_FORCE_RENDER=1`. Without it the occlusion gate
tears the decoder down and every reading comes back as the idle baseline —
which is correct behaviour and a useless measurement.

## How it stays cheap

**Occlusion tears down, it does not pause.** When nothing on an output can see
the wallpaper, the codec context, the VA-API surfaces and the demuxer are
*destroyed*. What stays resident is a timestamp. The last frame remains in the
surface the compositor is already showing, and playback resumes from that
timestamp when the desktop reappears. Teardown is delayed 0.4 s so a window
animation or a workspace switch does not thrash it; re-activation is immediate.

**Partial coverage counts too** (X11). A window covering all but a corner of the
desktop is not "visible" in any useful sense. The uncovered fraction is derived
from the window list on a 64×40 grid; rendering stops below 8% and resumes above
15%. The gap between the two is deliberate — a single threshold makes a window
edge resting near it toggle the decoder repeatedly.

Only ordinary application windows count. `_NET_WM_WINDOW_TYPE_DESKTOP`, `_DOCK`
and `_NOTIFICATION` are excluded, windows on another virtual desktop are
excluded, and anything under 95% opaque is excluded — over-estimating what is
visible keeps rendering, which is the safe direction to be wrong.

**One frame in flight.** Each tick pulls exactly one frame and draws it. No
read-ahead queue, no synchroniser with a timebase, and `thread_count = 1` on the
decoder — frame-threaded decoding deliberately keeps several frames in flight to
have work for each thread, which is the opposite of what this wants. The macOS
port measured the read-ahead alternative at 28 MB against 19 MB for no CPU
improvement.

**Zero-copy where the hardware allows it.** VA-API decodes into a surface
exported as a dmabuf, imported as an `EGLImage` and sampled as a
`GL_TEXTURE_EXTERNAL_OES`. The frame never enters system memory, and YUV-to-RGB
happens in fixed-function hardware on the sampler — which is also why there is
no colour matrix in the shader and no separate 10-bit variant. The decoder
already knows which matrix the file asked for.

Without VA-API or without `EGL_EXT_image_dma_buf_import`, frames are decoded in
software and converted with swscale before upload. `livewall status` reports
which path is live, because the difference is large enough that you should not
have to guess.

**One thread.** The frame pump, the control socket, both DBus connections and
the display server's events are all polled together, and the loop sleeps until
the earliest deadline any of them has. A thread that sleeps is still a thread
the scheduler wakes, and the only long-running operation — transcoding an import
— deliberately runs in the CLI process instead.

**Nothing is painted opaque.** Every surface is transparent where the wallpaper
is not, so "no wallpaper of ours" degrades to whatever is behind rather than to
a black rectangle, and Fit mode's letterbox shows your real desktop background.
On X11 that needs a compositing manager; without one the app detects it, picks a
non-alpha visual and says so in the log, and the bars are black.

## When rendering stops

| Signal | Where it comes from | Behaviour |
|---|---|---|
| Desktop fully covered | X11 window list | teardown after 0.4 s |
| Under 8% of the desktop uncovered | X11 window list | teardown; resumes above 15% |
| Compositor stopped asking for frames | Wayland frame callback | pump stalls |
| Screen locked | logind `LockedHint` | teardown |
| Screensaver active | `org.freedesktop.ScreenSaver` | teardown |
| Display asleep | X11 DPMS | teardown |
| System suspending | logind `PrepareForSleep` | teardown |
| Machine running hot | `/sys/class/thermal`, against each zone's own critical trip | teardown |
| Power-saver profile | power-profiles-daemon | teardown |
| Battery below 20% | `/sys/class/power_supply` | teardown |
| On battery at all | as above | optional, off by default |
| No input for 15 minutes | XScreenSaver extension, or logind `IdleHint` | teardown |

Every one of these is a hard stop rather than a slowdown, and that is forced by
the decode path: playback pulls one frame per tick and the assets have no
B-frames, so every frame is a reference frame that must be decoded whether or
not it is shown. Lowering the tick rate would play the clip in slow motion, not
more cheaply.

Thermal state is the one gate Linux does *better* than the other two platforms —
Windows exposes nothing equivalent to an unprivileged process, and the Windows
port says so. Each zone is measured against its own critical trip point rather
than a fixed temperature, so a laptop that idles at 85 °C by design is not
permanently gated.

`livewall status` lists any gate that could not be wired up on this machine:

```
power gates missing — power-profiles-daemon; battery (desktop machine)
```

A gate that silently does not exist is worse than one documented as missing.

## Import is mandatory

Videos are converted on import and only the converted file is ever played. This
is the trade that makes the runtime numbers predictable: the playback path never
sees an unknown codec.

| Source problem | Cost if played directly | Fixed at import |
|---|---|---|
| VP9 / AV1 / ProRes | software decode, tens of % CPU | transcoded to HEVC → the media engine |
| Source larger than the panel | decodes pixels you can't see | scaled to cover the display, never past 1:1 |
| 60 fps | 2.5× the pump work | capped, and snapped to a divisor of the refresh rate |
| Audio track | a second decoder for nothing | stripped |
| B-frames | decoder needs a reorder buffer | `max_b_frames = 0` |
| `moov` atom at EOF | full-file read before playback | `+faststart` |

Frame rate is snapped down to a rate that divides the display's refresh exactly:
24 fps divides 120 Hz evenly, but on a 60 Hz panel it doesn't, and a frame
arriving between two refreshes is decoded and then dropped by the compositor.

| Preset | Size | Rate | Depth |
|---|---|---|---|
| `ultra` | 960p | 20 fps | 8-bit |
| `balanced` (default) | 1920p | 24 fps | 8-bit |
| `native` | your display | 24 fps | 10-bit |

### Which encoder

The other two ports name HEVC and move on, because their OS guarantees an
encoder. A Linux distribution's FFmpeg build may have libx265, or only libx264,
or neither and a VA-API device instead — Debian and Ubuntu ship an LGPL build
without x264/x265 in some configurations.

So the encoder is chosen at import time, in this order, and the one that was
used is written into the index:

`libx265` → `libx264` → `hevc_vaapi` → `h264_vaapi` → `hevc_nvenc` →
`h264_nvenc` → `mpeg4`

Ordered by what the result costs to *play*, not by encode speed. An import
happens once and playback happens for months, so a slower encode that produces a
file the GPU decodes in fixed-function hardware wins every time. `livewall
probe` shows what this machine has.

Imports run in the CLI process, not the daemon — an import takes minutes and the
daemon's single thread is also every output's frame pump.

## Scaling

`fill` (default), `fit` or `stretch`. It is a property of the presentation
layer — four floats handed to the vertex shader — so switching is free. No
re-encode, no reload, nothing on disk changes.

Fit's letterbox is produced by sampling *past* the edge of the texture, which
the fragment shader turns transparent. Moving the vertices instead would leave
the bars showing whatever was in the framebuffer before.

## Install

```sh
./tools/install.sh                   # builds, installs to ~/.local, starts it
PREFIX=/usr/local sudo ./tools/install.sh
```

Build dependencies — headers and `wayland-scanner`, none of them needed at
runtime:

```sh
# Debian / Ubuntu
sudo apt install build-essential cmake ninja-build pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxfixes-dev \
    libegl-dev libgles-dev libwayland-dev wayland-protocols \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
    libva-dev libdbus-1-dev

# Fedora
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
    libX11-devel libXext-devel libXrandr-devel libXfixes-devel \
    mesa-libEGL-devel mesa-libGLES-devel wayland-devel \
    ffmpeg-free-devel libva-devel dbus-devel

# Arch
sudo pacman -S base-devel cmake ninja libx11 libxrandr libxfixes \
    mesa wayland ffmpeg libva dbus
```

Then:

```sh
livewall add ~/Videos/loop.mp4
livewall autostart on
```

Two samples to try it with are in [`Samples/`](../../Samples/) — generated from
the app's own procedural field, so they are original work under this
repository's licence, and they loop seamlessly (0.991 SSIM across the seam).

## Using it

```
livewall                       run the daemon (this is what autostart runs)

livewall status                what it is doing and why
livewall list                  the library; * marks what is playing
livewall play <id|title>       play a wallpaper (an id prefix is enough)
livewall stop                  back to the procedural gradient

livewall add <video> [preset]  import a video and play it
livewall remove <id|title>     delete a wallpaper and its file
livewall preset [name]         show or set the import preset

livewall fit fill|fit|stretch  how a frame maps onto a display
livewall battery on|off        pause rendering whenever on battery
livewall autostart [on|off]    start with the session

livewall convert <in> <out.mp4> [preset]   transcode without importing
livewall probe                 what this machine can and cannot do
livewall quit                  stop the daemon
```

The CLI is the interface and the tray is a convenience over it. That is the
opposite of the macOS and Windows ports, where the menu bar item and the tray
*are* the whole UI — but a Linux desktop may have no tray at all, and the ones
that do disagree about what a tray item may do. Making the CLI primary also
means the app is scriptable and works over ssh.

`LIVEWALL_VERBOSE=1` makes the daemon log every gate transition.

### Where things go

| | |
|---|---|
| `$XDG_DATA_HOME/livewall/library/` | transcoded files |
| `$XDG_DATA_HOME/livewall/index.json` | the library index |
| `$XDG_CONFIG_HOME/livewall/settings.json` | settings |
| `$XDG_RUNTIME_DIR/livewall.sock` | the control socket |
| `$XDG_CONFIG_HOME/autostart/livewall.desktop` | the autostart entry |

Split across the data and config trees rather than kept together as on the other
two platforms. `$XDG_DATA_HOME` is what a backup tool copies and
`$XDG_CONFIG_HOME` is what a dotfile repo tracks, and a few hundred megabytes of
transcoded video belongs in neither by accident.

The index format is shared with the macOS and Windows ports — same field names,
same uppercase-hyphenated ids, same ISO 8601 timestamps. The media files are not
portable between them; the index is, and that costs nothing to preserve.

## Build without installing

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default              # 10 suites, no display or GPU needed
./out/build/default/livewall probe

cmake --preset no-wayland           # X11 only, for a machine with no libwayland
```

## Layout

```
src/
  platform/   Backend interface; x11/ (override-redirect, lowered, click-through)
              wayland/ (wlr-layer-shell background layer)
  render/     EglDevice, GlProgram, GradientSource, VideoSource, FitMode, shaders/
  import/     FFmpeg (dlopen), CodecSupport, Transcoder, FramePacer, Library
  app/        AppHost (the event loop), WallpaperEngine, OutputController,
              ControlSocket, TrayIcon, Cli
  support/    Log, Strings, Json, Guid, Paths, Settings, Footprint,
              Dynamic (dlopen), DBus, DesktopVisibility, PowerMonitor, StartupItem
protocol/     three vendored Wayland protocol definitions
tests/        coverage geometry, frame pacing, output sizing, index decoding
```

On X11 the wallpaper is an **override-redirect** window, lowered to the bottom
of the stack, with an empty XFixes input region. Unmanaged rather than a managed
`_NET_WM_WINDOW_TYPE_DESKTOP` window because window managers place managed
desktop windows themselves and most force them to the full screen area — which
is wrong the moment there is a second monitor, since this port draws one window
per output.

## Known limits

- **No hardware measurements.** See above. Every cost claim in this file is
  either structural or carried over from the macOS port's measurements; none of
  the memory or CPU numbers has been taken on a real Linux GPU.

- **The tray has no menu.** A StatusNotifierItem's menu is a second protocol,
  `com.canonical.dbusmenu`, whose `GetLayout` returns a recursively nested
  `(ia{sv}av)`. Supporting it means a general DBus marshaller, and this app's
  wrapper handles a deliberately fixed set of shapes. So: left click cycles
  wallpapers, middle click cycles fit mode, the title carries the status line,
  and everything else is the CLI.

- **Rotation metadata is ignored.** A video shot in portrait on a phone carries
  a display matrix that this pipeline does not apply, so it imports unrotated.
  Fixing it properly needs libavfilter or a rotation term in the fit transform;
  neither is here yet.

- **A desktop-icon layer can cover it.** GNOME's Nautilus and KDE's Plasma both
  draw the desktop as their own full-screen window. On X11 the wallpaper sits
  below that window, so it is hidden. Window managers where the desktop *is* the
  root window — i3, bspwm, openbox, XFCE with desktop icons off — show it
  correctly. This is the same shape of problem the Windows port has with
  `WorkerW`.

- **Multi-display is written for but barely exercised.** One surface, one
  decoder and one occlusion latch per output, and none of it has run on a real
  second monitor.

- **Each display decodes independently.** A two-monitor setup runs two decoders.
  Sharing one would be cheaper only while both are visible; separate sources let
  a covered display tear down on its own.

- **The VA-API path is untested on real hardware.** The container has no render
  node, so every run so far has taken the software fallback. The dmabuf import
  falls back rather than failing if anything about it is wrong, which limits the
  blast radius but is not the same as having seen it work.

- **A multi-layer dmabuf descriptor falls back to the upload path.** Some
  drivers export NV12 as separate R8 and GR88 images, which needs two textures
  and a hand-picked colour matrix in the shader. Falling back is correct without
  making that choice.

- **`livewall add` and the daemon both write the library.** The import runs in
  the CLI process and the daemon re-reads the index afterwards. Both writes are
  atomic, but running two imports at once is not something anything guards
  against.

- **The thermal, low-battery and idle gates are coded but not observed firing.**
  The conditions are awkward to stage deliberately — the same caveat the macOS
  README carries.

- **No sandboxing story.** No Flatpak manifest, no AppArmor profile. The daemon
  needs the GPU, the display socket and the user's own files, and nothing has
  been done to constrain it beyond that.
