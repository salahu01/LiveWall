<img src="docs/icon.png" width="128" align="right" alt="LiveWall icon">

# LiveWall

A live wallpaper app built around one constraint: **it should cost almost nothing
when you aren't looking at it.**

Existing video wallpaper apps mostly wrap a media player around whatever file you
drop in and leave it decoding whether or not the desktop is visible. That is why
they show up in Activity Monitor and Task Manager, and why laptop fans spin.
LiveWall takes the opposite approach — every design decision trades features for
resources.

Status bar or notification area only. No window, no dock or taskbar item, no
dependencies.

| | |
|---|---|
| **[macOS](platforms/macos/README.md)** | Swift, AppKit, AVFoundation, Metal. 504 KB binary, 12 MB idle. |
| **[Windows](platforms/windows/README.md)** | C++20, Win32, Direct3D 11, Media Foundation. Same design, same size class. |

Each platform's README is the real documentation — how it stays cheap, what was
measured, and what it gives up. This page covers what the two share.

## The design, in five decisions

Both apps are the same program in two languages, and these are the decisions that
make them one project rather than two:

**Visibility tears down, it does not pause.** When nothing on a display can see
the wallpaper, the decoder and its decode session are *destroyed*, not suspended.
The last frame stays composited by the window server at no cost, and playback
resumes from the saved timestamp. Every stop condition — a covered desktop, a
locked screen, a flat battery, an absent user — is a hard stop rather than a
slowdown, because the decode path pulls one frame per tick over B-frame-free
assets: slowing the tick rate would play the clip in slow motion, not more
cheaply.

**Partial coverage counts.** A window covering all but a corner of the desktop
still reports the wallpaper as "visible" to both operating systems, and the naive
version decoded at full rate for that corner. Both apps derive the uncovered
fraction from the window list on a 64×40 grid, stop below 8% and resume above
15%. The gap between the two thresholds is deliberate: with one threshold, a
window edge resting on it toggles the decoder repeatedly.

**One frame in flight.** Exactly one decoded frame is pulled per tick and handed
straight to the compositor. There is no read-ahead queue and no media clock,
because the obvious alternative on each platform — `requestMediaDataWhenReady`
with a render synchroniser, `IMFSourceReader` in async mode — reads ahead by an
amount you cannot control, and one 4K 10-bit frame is ~20 MB.

**Never ask the decoder for a format it isn't already producing.** Both platforms
punish it: AVFoundation revalidates every buffer through a synchronous kernel
round trip, Media Foundation inserts a whole extra transform with its own frame
pool. Take the decoder's native 4:2:0 output and convert in the compositor or a
pixel shader, which is free.

**Import is mandatory.** Videos are converted on import and only the converted
file is ever played, so the playback path never meets an unknown codec, a frame
larger than the panel, a 60 fps cadence, an audio track or a B-frame. Your
original is never copied or modified.

## Samples

Two loops to try it with, in [`Samples/`](Samples/). Generated from the app's own
procedural field rather than sourced, so they are original work under this
repository's licence — and they loop seamlessly, measured at **0.991** SSIM
across the seam.

| ![aurora](docs/sample-aurora.jpg) | ![ember](docs/sample-ember.jpg) |
|---|---|
| `Samples/aurora.mp4` | `Samples/ember.mp4` |

Regenerate or retune with [`tools/make-samples.sh`](tools/make-samples.sh) (needs
`ffmpeg`; the samples are shared by both platforms).

## What the resolution and bitrate actually cost

Measured on macOS (M3 Pro) against the real playback path, same clip, one
variable at a time. The Windows decode path is deliberately the same design, and
the conclusion is what both apps' presets are built on:

| Variant | Pixels | Memory | CPU |
|---|---|---|---|
| 1280×720, 1.4 Mbps, 8-bit | 1.0× | 15 MB | 3.00% |
| 1920×1080, 3.1 Mbps, 8-bit | 2.3× | 16 MB | 3.14% |
| 1920×1080, 6.8 Mbps, 8-bit | 2.3× | 18 MB | 2.74% |
| 3492×1964, 10 Mbps, 8-bit | 7.4× | 18 MB | 3.44% |
| 1920×1080, 5.6 Mbps, 10-bit | 2.3× | 18 MB | 2.92% |
| 1920×1080, 6.8 Mbps, **60 fps** | 2.3× | 19 MB | **7.78%** |

**Pixels are free, bits are free, frames are expensive.** Hardware decode is
fixed-function and flat in frame size; what remains is the app's own per-frame
pump work, which scales with frames per second and nothing else. So the presets
spend freely on resolution, bit depth and bitrate, and stay frugal with frame
rate.

Reproduce with `platforms/macos/tools/measure.sh` or
`platforms/windows/tools/measure.ps1`.

## Where the two differ

The ports are honest about what one platform makes easy and the other does not.
In full in each README; in brief:

| | macOS | Windows |
|---|---|---|
| Sitting behind desktop icons | `kCGDesktopWindowLevel`, documented, one call | undocumented `WorkerW` parenting via message `0x052C`, verified with a fallback |
| Occlusion signal | `NSWindow.occlusionState` fires on its own; the window list only refines it | no signal at all for a WorkerW child — the window-list poll *is* the gate |
| Procedural mode | `CAGradientLayer`, interpolated by the render server, costs the app zero | a 15 fps pixel shader; no compositor-side gradient exists |
| Thermal gate | `ProcessInfo.thermalState` | none — Windows has no guaranteed API. Battery Saver is the proxy |
| Codec | HEVC Main10 guaranteed on every Mac | probed, with an HEVC Main10 → HEVC Main → H.264 fallback ladder |
| Launch at login | `SMAppService`, bound to bundle identity, breaks silently on rebuild | `HKCU\...\Run`, stores a path, repaired automatically on next launch |

## Layout

```
platforms/macos/      Swift package, tools/, Resources/, Tests/
platforms/windows/    CMake project, src/, tools/, res/, tests/
docs/                 shared images
Samples/              shared sample loops
tools/                shared: make-samples.sh
```

## Licence

MIT. See [LICENSE](LICENSE).
