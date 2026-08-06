# Sample wallpapers

Two ready-to-import loops. Add either with **Add Video…** in the status menu.

| | |
|---|---|
| ![aurora](../docs/sample-aurora.jpg) | ![ember](../docs/sample-ember.jpg) |
| **aurora.mp4** — 1920×1080, 24 fps, 10 s | **ember.mp4** — 1920×1080, 24 fps, 10 s |

## Where they come from

Generated, not filmed or downloaded — three sinusoids crossed into a drifting
field, the same construction as the procedural mode in
[`Resources/wallpaper.metal`](../Resources/wallpaper.metal), rendered to video
with `ffmpeg`'s `geq` filter. Regenerate or retune them with
[`tools/make-samples.sh`](../tools/make-samples.sh).

That means they are original work under this repository's MIT licence, with no
third-party footage to attribute — which is the whole reason they exist rather
than a couple of clips pulled off the internet.

## They loop seamlessly

Every time term completes a whole number of cycles over the clip's ten seconds,
so the last frame joins the first with no visible cut. Measured with SSIM
between the last frame and the first:

| Clip | Loop seam | Mid-clip control |
|---|---|---|
| aurora | **0.991** | 0.748 |
| ember | **0.991** | 0.789 |

The control is a frame from the middle of the clip compared against the first
one — what a *bad* seam would score.

## What import does to them

```
aurora.mp4 → 1920×1080 @ 24 fps, 10-bit HEVC, 1.6 MB
ember.mp4  → 1920×1080 @ 24 fps, 10-bit HEVC, 1.6 MB
```

They stay at 1920×1080 on a 3024×1964 panel because import never upscales past
the source — there is no detail above it to recover, and inventing pixels only
costs memory. A 4K source on the same display would come out at 3492×1964.
