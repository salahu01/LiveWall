<img src="../../docs/icon.png" width="128" align="right" alt="LiveWall icon">

# LiveWall for Windows

A live wallpaper app for Windows built around one constraint: **it should cost
almost nothing when you aren't looking at it.**

This is a port of [the macOS app](../macos/README.md), and it keeps that app's
design rather than its code: hardware decode with exactly one frame in flight,
resources torn down rather than paused when nothing can see the desktop, and a
mandatory import step so the playback path never meets an unknown codec. The
[top-level README](../../README.md) covers what the two share.

Notification area only. No window, no taskbar button, no dependencies beyond the
Windows SDK — the tray item is the entire interface.

## Requirements

- Windows 10 1809 or later (Windows 11 is fine)
- A GPU with a Direct3D 11 driver, which every machine of the last decade has
- To build: Visual Studio 2022 with **Desktop development with C++**, or the
  Build Tools. Nothing else — no vcpkg, no NuGet, no submodules.

## Install

```powershell
./tools/install.ps1
```

Then use **Add Video…** in the tray menu.

**Install it before enabling Start with Windows.** The Run key stores the *path*
of the executable. Point it at a build directory and it works until that
directory is cleaned or moved, at which point Windows silently starts nothing at
login. An installed copy is stable. (The app repairs a stale registration on its
next launch — but only if it is launched, which after a failed login-start it
will not be.)

Installs to `%LOCALAPPDATA%\Programs\LiveWall`, per user. Nothing here needs
administrator rights: the app writes only to `%LOCALAPPDATA%` and `HKCU`.

## Build without installing

```powershell
./tools/build.ps1                    # configure + build Release
./tools/build.ps1 -Test              # ...and run the suite
./tools/build.ps1 -Configuration Debug

ctest --preset default --output-on-failure    # 45 tests, no display or GPU needed
./tools/measure.ps1 20                        # sample memory and CPU of the running app
./tools/make-icon.ps1                         # regenerate res/*.ico
```

```powershell
# verbose logging to stderr and the debugger — logs every gate transition
$env:LIVEWALL_VERBOSE = 1; ./out/build/default/LiveWall.exe

# render even when the desktop is covered, so cost can be measured on a
# machine whose desktop is behind a terminal
$env:LIVEWALL_FORCE_RENDER = 1; ./out/build/default/LiveWall.exe

# convert without the UI
./out/build/default/LiveWall.exe --convert in.mp4 out.mp4 native

# report what this machine can encode and decode — the first thing to ask when
# an import produces H.264 rather than HEVC
./out/build/default/LiveWall.exe --probe
```

## How it stays cheap

Everything below is the macOS design, restated in the APIs Windows actually has.
Where the two diverge, the difference is called out rather than glossed.

**Coverage tears down, it does not pause.** When nothing on a display can see the
wallpaper, the `IMFSourceReader` and its DXVA decode session are *destroyed*, not
suspended. The last presented frame stays in the swap chain's front buffer at no
cost, and playback resumes from the saved timestamp when the desktop reappears.
Teardown is delayed 0.4 s so window animations and Task View don't thrash it;
re-activation is immediate.

**The coverage gate is the only gate.** On macOS `NSWindow.occlusionState` is a
cheap yes/no that fires on its own, and the window-list walk only refines it.
Windows offers no equivalent for a window parented into WorkerW — it is behind
the desktop icons by construction, so the DWM never considers it occluded and
never has anything to report. So the window list is not an optimisation here; it
is the whole signal. It is polled every 4 seconds, which is cheap enough that a
wallpaper taking four seconds to notice it has been covered costs nothing anyone
can perceive. Coverage is measured on a 64×40 grid — ~1% precision, a few hundred
integer operations — because the answer feeds a threshold comparison. Stops below
8% uncovered, resumes above 15%; the gap is deliberate, since a single threshold
makes a window edge resting on it toggle the decoder repeatedly.

**One frame in flight.** The swap chain is created with
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` and a maximum frame latency
of 1. The render thread waits on that object, pulls exactly one sample, draws it
and presents. There is no read-ahead queue and no media clock.

The obvious alternative is `IMFSourceReader` in asynchronous callback mode, which
is this API's `AVPlayer`: it reads ahead by an amount you cannot control, and at
these resolutions one 10-bit frame is ~20 MB. Same conclusion the macOS version
reached about `requestMediaDataWhenReady`, for the same reason.

**Never ask for a pixel format the decoder isn't already producing.**
`MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` is explicitly **off** on the
playback path. Turning it on inserts the video processor MFT to hand back RGB,
which allocates a second full-resolution pool and, on machines where it misses
the fixed-function path, costs more CPU than the decode. Instead the reader is
asked for NV12 (8-bit) or P010 (10-bit) — exactly what the DXVA decoder already
emits — and YUV→RGB happens in a pixel shader, four multiply-adds on hardware
that has nothing else to do. This is the Windows shape of the macOS finding that
naming a pixel format made AVFoundation revalidate every buffer through a kernel
round trip.

The import path turns the same flag **on**, because there it is exactly what is
wanted: the downscale and any rotation, on the GPU, once.

**Frames are never copied to system memory.** The reader is given an
`IMFDXGIDeviceManager` bound to the app's own D3D device, so decoded frames
arrive as `ID3D11Texture2D` slices that are drawn directly. If a file ever comes
back as a system-memory buffer, that means DXVA did not engage — the app stops
and says so rather than quietly running a software decoder.

**Presenting costs no timer when the maths works out.** If the file's frame rate
divides the display's refresh rate exactly, `Present` is called with a sync
interval of `refresh / fps` and blocks in the driver until the right vertical
blank — no timer, no wake-up, no drift. The transcoder arranges for this by
snapping every import's frame rate to a divisor of the display it was imported
on. When it doesn't work out (a file imported on a 120 Hz laptop, later shown on
a 60 Hz monitor), a high-resolution waitable timer takes over.

**Shaders are compiled at build time.** `fxc` runs during the build and the byte
code is embedded. Compiling at runtime pulls the shader compiler into the
process; the macOS equivalent measured at ~97 MB of resident graphics memory
that is never released.

**Nothing is painted opaque.** The desktop window is created with
`WS_EX_NOREDIRECTIONBITMAP` and its content comes from a DirectComposition
visual holding a swap chain with premultiplied alpha, cleared to fully
transparent every frame. So "no wallpaper of ours" degrades to the system's own
picture rather than to a black rectangle, and in Fit mode the letterbox bars show
your real wallpaper. An ordinary HWND swap chain is opaque and could not do this.

## Sitting behind the desktop icons

macOS has a documented window level for exactly this: `kCGDesktopWindowLevel`
sits above the desktop picture and below the icons, with one API call.

**Windows has no window level below the desktop icons at all.** What it has is
Explorer's own window arrangement, and the undocumented message that produces it:

```
Progman  ("Program Manager")
  ├─ WorkerW                  ← wallpaper is painted here
  ├─ SHELLDLL_DefView         ← the icon view
  │    └─ SysListView32
  └─ WorkerW                  ← a second, empty one
```

Sending `Progman` message `0x052C` makes it split its wallpaper rendering into a
`WorkerW` behind `SHELLDLL_DefView`. A child window parented into that `WorkerW`
draws over the wallpaper and under the icons, is never a target for clicks or
Alt-Tab, and does not appear in the window list. Every live-wallpaper app on
Windows does this, and there is no supported alternative.

Being honest about what it costs:

- **It is undocumented, so it can change.** The code verifies the arrangement it
  got rather than assuming, and falls back to parenting into `Progman` itself —
  which still works but draws *over* the desktop icons. The tray menu says so
  when that happens.
- **Explorer restarting destroys the whole tree.** The app listens for the
  `TaskbarCreated` broadcast and rebuilds every desktop window and the tray icon.
- **There is one `WorkerW` for the whole virtual desktop**, not one per monitor,
  so per-monitor windows are children positioned in virtual-screen coordinates.

## When rendering stops

| Signal | Behaviour |
|---|---|
| Under 8% of the desktop uncovered | teardown after 0.4 s; resumes above 15% |
| Session locked, or a disconnect | teardown |
| Display turned off (`GUID_SESSION_DISPLAY_STATUS`) | teardown |
| Screen saver running | teardown |
| A full-screen app or presentation mode | teardown |
| Battery Saver on | teardown |
| Battery below 20% | teardown |
| On battery at all | optional, off by default |
| No input for 15 minutes | teardown |

Every one of these is a hard stop rather than a slowdown, and that is forced by
the decode path: playback pulls one frame per tick and the assets have no
B-frames, so every frame is a reference frame that must be decoded whether or not
it is shown. Lowering the tick rate would play the clip in slow motion, not more
cheaply. Stopping is both free and graceful — the last frame stays on screen.

**Two differences from the macOS gate list, both stated rather than hidden:**

*There is no thermal gate.* macOS has `ProcessInfo.thermalState`; Windows exposes
thermal information only through vendor drivers and WMI classes that are not
present on every machine, and polling WMI would cost more than the wallpaper
does. Battery Saver is the closest signal the OS actually guarantees, and it is
what the gate uses.

*Full-screen apps are a machine-wide gate, not a coverage one.* An exclusive-mode
Direct3D swapchain may not appear in the window list at all, so the coverage grid
would report the desktop as wide open behind a running game.
`SHQueryUserNotificationState` catches it.

## Import is mandatory

Videos are converted on import and only the converted file is ever played. This
is the trade that makes the runtime numbers predictable: the playback path never
sees an unknown codec.

| Source problem | Cost if played directly | Fixed at import |
|---|---|---|
| VP9 / AV1 / ProRes | software decode, tens of % CPU | transcoded to HEVC or H.264 → GPU |
| Source larger than the panel | decodes pixels you can't see | scaled to cover the display, never past 1:1 |
| 60 fps | 2.5× the pump work | capped, and snapped to a divisor of the refresh rate |
| Audio track | Media Foundation spins up an audio decoder | stream deselected |
| B-frames | decoder needs a reorder buffer | `AVEncMPVDefaultBPictureCount = 0` |
| `moov` atom at EOF | full-file read before playback | `MF_MPEG4SINK_MOOV_BEFORE_MDAT` |

Presets:

| Preset | Size | Rate | Depth |
|---|---|---|---|
| Ultra Light | 960p | 20 fps | 8-bit |
| Balanced (default) | 1920p | 24 fps | 8-bit |
| Native | your display | 24 fps | 10-bit |

Conversion uses `IMFSourceReader` → `IMFSinkWriter` with the driver's hardware
encoder — no bundled `ffmpeg`, which would add 40–70 MB and a licensing question
to a half-megabyte app. Your original file is never copied or modified.

### The codec problem, which macOS does not have

On macOS, HEVC is guaranteed: every Mac that runs the app has a hardware HEVC
encoder and decoder, so the transcoder can simply demand HEVC Main10.

On Windows neither half holds:

- **The encoder comes from the graphics driver.** Quick Sync, NVENC and VCE all
  supply one, but a machine on a basic display driver, a VM, or a remote session
  has none.
- **The decoder is worse.** Microsoft's HEVC decoder is not in a stock Windows
  install — it is the paid *HEVC Video Extensions* from the Store, or an OEM
  variant. Hardware HEVC decode through the driver's own MFT usually works
  without it, but "usually" is not something to build a *mandatory* import path
  on.

Writing a file the machine cannot play back is the one failure this app must not
have, because import is mandatory and an unplayable output means a wallpaper that
silently never appears. So both halves are probed before choosing, and the answer
falls back along a ladder:

```
HEVC Main10 (10-bit)  →  HEVC Main (8-bit)  →  H.264 High (8-bit)
```

H.264 is a safe floor: every Windows since 7 has a decoder and every GPU of the
last decade decodes it in fixed function. It costs roughly 40% more bitrate for
the same quality — which is disk space and nothing else, since the playback
numbers are set by frame rate, not by bits.

`LiveWall.exe --probe` reports what your machine can do, and the Import Quality
submenu says so too when it has had to fall back.

## Scaling

Fill (default), Fit or Stretch, switchable from the tray menu. It is a property
of the vertex shader's transform, so switching is free — no re-encode, no reload,
nothing on disk changes. Each mode's menu label states what it costs for the
wallpaper and display you actually have, e.g. *"Crops 13% of the width."* (macOS
puts that in a tooltip; Win32 menu items have none, so it goes in the label.)

## Layout

```
platforms/windows/
  src/
    app/        AppHost (tray + message loop), TrayMenu, WallpaperEngine,
                MonitorController
    render/     DesktopHost (WorkerW), D3DDevice, SwapChainTarget, VideoSource,
                GradientSource, FitMode, shaders/wallpaper.hlsl
    import/     Transcoder, FramePacer, CodecSupport, Library
    support/    PowerMonitor, DesktopVisibility, StartupItem, Settings, Paths,
                Json, Strings, Guid, Footprint, Log
  tests/        coverage geometry, frame pacing, output sizing, index decoding
  res/          manifest, version info, icons
  tools/        build.ps1, install.ps1, measure.ps1, make-icon.ps1
```

One window per monitor, each with independent coverage state, all parented into
the single `WorkerW` that covers the virtual desktop. One shared `ID3D11Device`;
one swap chain and one decoder per monitor.

Settings live in `%LOCALAPPDATA%\LiveWall\settings.json` and the library index in
`index.json` beside it — not in `%APPDATA%`, because a roaming profile would drag
a folder of transcoded video across the network at every sign-in. The one thing
in the registry is the Run key, because that is where Windows looks.

The index format is byte-for-byte the one the macOS app writes, including the
optional `bitDepth` field. Nothing depends on that today, but the two apps are
the same project and an index only one of them could read would be a decision
made by accident.

## Known limits

- **Each display decodes independently.** A two-monitor setup runs two decoders.
  Sharing one would be cheaper only while both are visible; separate sources let
  a covered display tear down on its own. Worth revisiting for 3+ displays.
- **The procedural mode is not free here, unlike on macOS.** There, it is a
  `CAGradientLayer` the compositor interpolates, costing the app literally zero.
  DirectComposition can animate a transform or an opacity but not a gradient's
  colour stops, and there is no compositor-side shader — so this pays a real
  cost: one full-screen triangle at 15 fps.
- **The WorkerW arrangement is undocumented.** Verified rather than assumed, with
  a stated fallback, but a future Windows build could change it.
- **No thermal gate.** See above.
- **10-bit is not guaranteed.** See the codec ladder above.
- **One wallpaper on all displays.** No per-display assignment, no playlist.
- **The published numbers in this file are the macOS app's**, restated for a
  design that is deliberately the same. The Windows build has not been measured
  on a range of hardware, and the two platforms' decode paths are close but not
  identical — run `tools/measure.ps1` on your own machine before quoting a figure
  from here.
- **Multi-monitor is written for and lightly exercised.** Mixed-DPI and
  mixed-refresh setups in particular deserve more than they have had.
- **Not signed.** SmartScreen will warn on first run of an unsigned executable.
  Signing needs a code-signing certificate; there is no ad-hoc equivalent of the
  macOS local signature.
