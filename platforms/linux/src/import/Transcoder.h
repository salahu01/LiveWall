// Normalises an arbitrary user video into a wallpaper-shaped asset.
//
// Import is mandatory, not an optimisation pass — the playback path only ever
// sees files that came out of here. That is what makes the runtime numbers
// predictable regardless of what the user dragged in.
//
// What each step buys, on this platform specifically:
//
//   Transcode. VP9, AV1 and ProRes are all common in the wild and all fall back
//   to software decode on most GPUs. Re-encoding to HEVC (or H.264 where there
//   is no HEVC encoder) puts playback on the media engine.
//   Downscale. A 4K source on a 1440p panel decodes four times the pixels
//   nobody sees, and frame memory scales with this directly.
//   Cap the frame rate. Ambient loops gain nothing above 24 fps and pay
//   linearly for every frame above it.
//   Drop the audio track. No audio stream means no audio decoder at playback.
//   No B-frames. The decoder needs no reorder buffer, which shrinks its surface
//   pool and removes decode latency on the resume-from-occlusion seek.
//   `+faststart`. Moves the moov atom to the front so opening the file does not
//   read to the end first — the same thing `shouldOptimizeForNetworkUse` does
//   on macOS and `MF_MPEG4SINK_MOOV_BEFORE_MDAT` does on Windows.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace livewall {

// Measured on the other two ports against their real playback paths, and the
// shape of the result is a property of video decoding rather than of any one
// operating system: going from 1280x720 to 3492x1964 (7.4x the pixels) moved
// the footprint 15 MB -> 18 MB and left CPU inside the noise, while going from
// 24 to 60 fps took CPU from 3.5% to 7.8%.
//
// Fixed-function decode is flat in frame size; what remains is the app's own
// per-frame pump work, which scales with frames per second and nothing else. So
// the presets spend freely on resolution, bit depth and bitrate — all close to
// free at playback — and stay frugal with frame rate, which is the only knob
// that costs.
struct TranscodePreset {
    std::string name;
    // Longest output edge in pixels, or 0 to size against the display.
    int maxEdge = 0;
    int fps = 24;
    // Bits per pixel per second. Costs disk and nothing else: on macOS the
    // 6.8 Mbps variant measured marginally *cheaper* to play than the 1.4 Mbps
    // one. A lower number starves the smooth gradient content that makes good
    // wallpaper, which is what banding in smoke and glow looks like.
    double bitsPerPixel = 0.15;
    // 10-bit costs nothing to decode and is the real fix for banding.
    int bitDepth = 8;
    // Keyframes are expensive at these bitrates and a wallpaper seeks only when
    // it resumes from occlusion, so they can be sparse.
    double keyframeSeconds = 5;

    std::string summary() const;
};

// What the output is sized and paced against, taken from the output the
// wallpaper will actually play on.
struct DisplayTarget {
    int pixelWidth = 1920;
    int pixelHeight = 1080;
    int refreshHz = 60;
};

struct TranscodeResult {
    std::string path;
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitDepth = 8;
    std::int64_t byteCount = 0;
    // "hevc", "h264" or "mpeg4" — whichever encoder this machine had. Written
    // into the index because, unlike the other two ports, it is not knowable in
    // advance.
    std::string codec;
};

class Transcoder {
public:
    using ProgressFn = std::function<void(double)>;

    static std::optional<TranscodeResult> convert(const std::string& source,
                                                  const std::string& destination,
                                                  const TranscodePreset& preset,
                                                  const DisplayTarget& display,
                                                  const ProgressFn& progress);

    static const std::vector<TranscodePreset>& presets();
    static const TranscodePreset& presetNamed(std::string_view name);
    static const TranscodePreset& defaultPreset();

    // --- pure, and tested ---------------------------------------------------

    // Output dimensions for a preset.
    //
    // A fixed `maxEdge` fits the source inside that edge. Zero sizes against
    // the display instead — scaled so the frame *covers* the panel, since
    // anything less is upscaled at playback and that upscale was the single
    // largest quality loss in the macOS pipeline. Neither path ever scales
    // above 1:1: the source has no detail past its own resolution and inventing
    // pixels only costs memory.
    static void outputSize(int sourceWidth, int sourceHeight, const TranscodePreset& preset,
                           const DisplayTarget& display, int* width, int* height);

    // Largest frame rate no greater than `preferred` that divides `refresh`
    // exactly. Falls back to `preferred` when nothing sensible divides it.
    //
    // A frame arriving between two refreshes is decoded and then dropped by the
    // compositor, which is the most wasteful thing this pipeline can do. 24
    // divides 120 exactly; on a 60 Hz panel it does not, and 20 is both
    // smoother and cheaper.
    static int pacedFps(int preferred, int refresh);
};

}  // namespace livewall
