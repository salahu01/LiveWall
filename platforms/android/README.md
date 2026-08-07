<img src="../../docs/icon.png" width="128" align="right" alt="LiveWall icon">

# LiveWall for Android

A live wallpaper built around one constraint: **it should cost almost nothing
when you aren't looking at it.**

The [macOS port](../macos/README.md) is where this design was worked out and
measured; the Windows port lives in [`platforms/windows/`](../windows/). This one
is the same argument on hardware that cares about it far more — a phone pays for
a busy wallpaper out of a battery it also has to run the day on.

Most live wallpapers wrap a player around whatever you hand them and leave it
decoding. LiveWall releases the decoder outright whenever nothing can see the
wallpaper, and the last frame stays on screen for free.

## What Android gives away, and what it charges for

Porting this design is mostly an exercise in deleting code, with one place where
it gets more expensive.

**Deleted.** On macOS, roughly 400 lines exist to put a window at
`kCGDesktopWindowLevel`, keep one per display, and derive from the window list
what fraction of the desktop is actually uncovered — because
`NSWindow.occlusionState` is a yes/no, and a window covering all but a corner
still reports the desktop as visible. Android replaces every bit of that with
`WallpaperService.Engine.onVisibilityChanged`, which is the system telling you
directly, and which already folds in the screen being off, the device being
locked, and any app being in front. The uncovered-fraction arithmetic, the 8%/15%
hysteresis, the window-level constants, the screen-lock and screensaver and
idle-timer observers, and the whole login-item story all simply have no
counterpart. [`LiveWallService.kt`](app/src/main/kotlin/com/fegno/livewall/app/LiveWallService.kt)
is 280 lines including its comments.

**Charged for.** The procedural gradient mode is not free here and is free
there. On macOS it hands `CAGradientLayer` keyframes to the render server once,
and WindowServer interpolates them on the GPU from then on — the app's own cost
is *literally* zero. SurfaceFlinger has no such facility: it composites the
buffer you posted and will not animate it for you. So the drift is redrawn from
the app process, at ten frames a second, and that is a real cost the macOS
version does not pay. It is a trivial shader on an idle GPU, but it is not
nothing, and the honest thing is to say so rather than quote the macOS number.

Two smaller ones. `Fit`'s letterbox bars are black rather than transparent —
macOS can show the system's own wallpaper through them, whereas on Android *this
is* the wallpaper and there is nothing underneath. And parallax on launcher
scroll is deliberately not implemented: it would mean redrawing at the panel's
refresh rate rather than the wallpaper's, which is the most expensive thing this
design could agree to do.

## Measured

Honest state of things: **these are emulator numbers, and the CPU figure is not
representative.** They come from an API 35 arm64 emulator with `hw.gpu.enabled=no`
— software AVC decode and a SwiftShader GL stack, i.e. every fixed-function
block this design depends on replaced by the CPU. Reproduce with
[`tools/measure.sh`](tools/measure.sh).

| State | Memory (PSS) | CPU |
|---|---|---|
| Video, 1920×1080 24 fps, wallpaper visible | 44 MB | 12.3% |
| Video, wallpaper hidden | 32 MB | **0.00%** |

The number that carries is the second row. **0.00% of one core, and 12 MB
returned**, because losing visibility releases the `MediaCodec` and its buffers
rather than pausing them. That is the entire claim, and it does not depend on
the codec being hardware.

The first row does. On a phone with a real media block, HEVC decode is
fixed-function and the CPU that remains is the app's own per-frame pump work —
which the macOS port measured at 2.9% of a core for a 24 fps clip, and which
scales with frames per second and nothing else. Expect something in that
neighbourhood; **no hardware-device numbers have been taken yet**, and none are
quoted here as if they had been.

Memory is also not comparable to the macOS figure. PSS includes the ART runtime,
the GL driver and the Kotlin stdlib, none of which the 12 MB Swift number
carries.

Release APK: **102 KB**. No AndroidX, no Compose, no Material, no media3, no
`ffmpeg` — the shipped runtime classpath is the Kotlin standard library and
nothing else.

## How it stays cheap

**Occlusion tears down, it does not pause.** When the wallpaper stops being
visible the `MediaCodec` and its buffer pool are *destroyed*, not suspended. The
last posted frame stays composited by SurfaceFlinger at no cost, and playback
resumes from the saved timestamp. Teardown is delayed 400 ms so the app-switch
and recents animations — which flap visibility in single-digit milliseconds —
don't thrash it; re-activation is immediate. This is visible in the log:

```
visibility -> false
video deactivated — 31.8 MB     # 400 ms later, 12 MB returned
visibility -> true
video activated at 8.458333s    # resumed, not restarted
```

**One frame in flight.** A `Choreographer` callback pulls exactly one decoded
frame per tick and draws it. There is no read-ahead queue and no media clock, so
the only decoded frames resident are the ones the codec's own output set holds.
An `ExoPlayer` or `MediaPlayer` on the same file buffers ahead by an amount the
app does not control, and at these resolutions one 10-bit frame is ~20 MB.

Android has no equivalent of pinning a display link to the asset's frame rate —
the callback arrives at the panel's refresh, whatever that is — so the rate cap
is a timestamp comparison at the top of the tick. The importer snaps the file's
frame rate to a divisor of the refresh precisely so that gate lands evenly.

**Never name a pixel format.** The decoder writes to a `SurfaceTexture` and is
sampled as a `GL_TEXTURE_EXTERNAL_OES`, so hardware HEVC hands back 4:2:0 at the
file's own bit depth — 1.5 bytes/px at 8-bit against RGBA's 4 — and the YUV→RGB
conversion happens in the sampler on the GPU's fixed-function path. Routing
through an `ImageReader` to name a format would add a full-frame copy per frame
for nothing. The macOS port reached the same conclusion from the opposite
direction, after profiling showed that naming a format cost a synchronous kernel
round trip per frame.

**The EGL context outlives teardown; the codec does not.** The decoder and its
buffers are megabytes per frame. The EGL context and one linked program are not —
and destroying the EGL surface would drop the last posted buffer, turning
"stopped wallpaper looks like a still" into "stopped wallpaper is black".

## When rendering stops

| Signal | Behaviour |
|---|---|
| Any app in front, screen off, device locked, launcher not showing | `onVisibilityChanged(false)` → teardown after 400 ms |
| Thermal status `SEVERE` or worse | teardown |
| Battery Saver | teardown |
| Battery below 20%, off charge | teardown |
| Not charging at all | optional, off by default |

The first row is one system callback and covers what took macOS an
uncovered-fraction calculation, a screen-lock observer, a display-sleep observer,
a screensaver observer and a fifteen-minute idle timer.

`THERMAL_STATUS_MODERATE` is deliberately *not* a stop. On a phone it is reached
by ordinary things like charging in a warm room, and stopping the wallpaper every
time the device warms up reads as a bug.

Every one of these is a hard stop rather than a slowdown, and that is forced by
the decode path: playback pulls one frame per tick and the assets have no
B-frames, so every frame is a reference frame that must be decoded whether or not
it is shown. Lowering the tick rate would play the clip in slow motion, not more
cheaply. Stopping is both free and graceful.

## Import is mandatory

Videos are converted on import and only the converted file is ever played. Your
original is never copied or modified. This is the trade that makes the runtime
numbers predictable: the playback path never sees an unknown codec.

| Source problem | Cost if played directly | Fixed at import |
|---|---|---|
| VP9 / AV1 on an SoC without the block | software decode, tens of % of a core, continuously, on a battery | transcoded to HEVC → media block |
| Source larger than the panel | decodes pixels you can't see | scaled to cover the display, never past 1:1 |
| 60 fps | 2.5× the pump work | capped, and snapped to a divisor of the refresh rate |
| Audio track | an audio graph at playback | not muxed |
| B-frames | decoder needs a reorder buffer | frame reordering disabled |

Frame rate is snapped down to a rate that divides the panel's refresh exactly: a
frame arriving between two refreshes is decoded and then dropped by the
compositor. This matters more on phones than it did on laptops — 90 Hz and 120 Hz
panels are ordinary, and 24 divides neither 90 nor 144.

Presets:

| Preset | Size | Rate | Depth |
|---|---|---|---|
| Ultra Light | 960p | 20 fps | 8-bit |
| Balanced (default) | 1920p | 24 fps | 8-bit |
| Native | your screen | 24 fps | 10-bit |

The default is one step cheaper than the macOS default, for the reason at the top
of this file.

The pipeline is `MediaExtractor` → hardware decoder → external texture → GL →
hardware encoder's input surface → `MediaMuxer`. Frames never touch the CPU: the
scale, the rotation and the format conversion all happen in the sampler. There is
no bundled `ffmpeg`, which would add tens of megabytes and a licensing question
to a 102 KB app, and would do the work on the CPU.

Two Android-specific things the macOS importer never had to handle. HEVC *encode*
is not guaranteed — decode has been mandatory since Android 5, encode has not —
so the importer falls back to AVC, which is hardware-decoded just the same.
And where an HEVC encoder exists it is sometimes a token one: Android's own
software fallback tops out at 512×512. Shrinking a 960p import to 512p to keep
the nicer codec is the wrong trade, so the importer takes AVC when it can hold
materially more picture. Ten-bit is requested only when both the encoder and an
EGL config can actually deliver it, and the library records what the file really
is rather than what was asked for.

## Scaling

Fill (default), Fit or Stretch. It is a two-component scale on the quad, so
switching is one uniform write — no re-encode, no reload, no decoder
involvement. Each mode states what it costs for the wallpaper and panel you
actually have, e.g. *"Crops 75% of the width."* On a 20:9 phone showing 16:9
footage that number is worth seeing before you wonder why your wallpaper looks
cropped.

## Install

```sh
./tools/build.sh install     # debug APK onto the attached device, then opens
                             # the live-wallpaper preview
```

Then **Add video…** in the app.

## Build and test

```sh
./tools/build.sh             # debug APK + unit tests
./tools/build.sh release     # minified release APK

./gradlew :app:testDebugUnitTest          # 55 tests, no device needed
./gradlew :app:connectedDebugAndroidTest  # 2 tests, needs a device

./tools/measure.sh 10        # memory and CPU of the running wallpaper

# verbose logging — every gate transition, every activate/deactivate
adb shell setprop log.tag.LiveWall VERBOSE && adb logcat -s LiveWall
```

The unit suite is pure logic — frame pacing, output sizing, fit geometry, index
decoding — so it needs no device, no GPU, no media files and no Robolectric. The
transcode path is the one thing that cannot be tested that way, because it is an
encoder, a muxer and an EGL context, and all three belong to the device; that
test lives in `androidTest` and asserts the shape of the output rather than its
timing.

### Signing

A release build signs itself only if it is pointed at a keystore, and nothing
about that keystore lives in this repository. Write the four properties
somewhere outside the tree:

```properties
storePassword=…
keyPassword=…
keyAlias=livewall
storeFile=/absolute/path/to/livewall.keystore
```

and name that file with either the `livewall.keyProperties` Gradle property —
`~/.gradle/gradle.properties` is the usual home for it — or the
`LIVEWALL_KEY_PROPERTIES` environment variable. A gitignored `key.properties`
beside `settings.gradle.kts` works too. The output is then
`app/build/outputs/apk/release/app-release.apk`, signed with v3.

With no keystore configured — a fresh clone, and CI — the release build still
succeeds and produces `app-release-unsigned.apk`, which cannot be installed
until you sign it:

```sh
apksigner sign --ks <keystore> --out LiveWall.apk \
    app/build/outputs/apk/release/app-release-unsigned.apk
```

## Layout

```
app/src/main/kotlin/com/fegno/livewall/
  app/        LiveWallService — the WallpaperService and its Engine
  render/     VideoSource, GradientSource, RenderTarget, EglCore,
              GlPrograms, FitMode
  importer/   Transcoder, FramePacer, Sizing, Library, LibraryIndex, Preset
  support/    PowerGate, Settings, Json, Displays, Log, Footprint
  ui/         SettingsActivity
app/src/test/       frame pacing, output sizing, fit geometry, index decoding
app/src/androidTest/ the transcode pipeline, on a device
```

Two threads do the work. `livewall-render` owns the EGL context, the GL calls,
the `Choreographer` and the codec. `livewall-frames` exists only because
`SurfaceTexture.onFrameAvailable` has to be delivered somewhere other than the
thread waiting for it, or the wait deadlocks the delivery.

## Known limits

- **No hardware-device measurements.** Everything in *Measured* above came off an
  emulator with software codecs. The teardown result is codec-independent and
  stands; the visible-state CPU figure does not, and no real-phone number is
  quoted as though it had been taken.
- **OEM skins are untested.** Some launchers keep the wallpaper permanently
  hidden behind a blur layer, some never report visibility correctly, and some
  aggressively kill wallpaper services. None of this has been exercised beyond
  stock Android.
- **Resume seeks to the preceding sync frame** and decodes forward to the saved
  timestamp with rendering off. Keyframes are five seconds apart, so that is up
  to ~120 frames of catch-up, bounded to 64 decode steps per tick — two or three
  ticks, not a stall. It is more work than the macOS port's exact
  `AVAssetReader` time range does.
- **The gradient mode is not free**, unlike its macOS counterpart. See the top of
  this file.
- **No parallax on launcher scroll**, deliberately.
- **One wallpaper**, no playlist, no separate lock-screen wallpaper.
- **The thermal and low-battery gates are coded but not observed firing** — the
  conditions are awkward to stage deliberately.
- **10-bit is best-effort.** It needs both an encoder advertising HEVC Main10 and
  an EGL config with 10 bits per channel; where either is missing the import
  quietly produces 8-bit and records that.
