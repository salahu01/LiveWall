// Normalises an arbitrary user video into a wallpaper-shaped asset.
//
// Import is mandatory, not an optimisation pass — the playback path only ever
// sees files that came out of here. That is what makes the runtime numbers
// predictable regardless of what the user dragged in.
//
// What each step is buying, and every one of these was measured on the macOS
// side against the real playback path:
//
//  - **Transcode to HEVC (or H.264).** VP9, AV1 and ProRes sources fall back to
//    software decode and cost tens of percent of a core. A codec the GPU
//    decodes in fixed function costs about 1%.
//  - **Downscale.** A 4K source on a 1440p display decodes four times the
//    pixels nobody sees. Frame memory scales with this directly.
//  - **Cap the frame rate.** Ambient loops gain nothing above 24 fps and pay
//    linearly for every frame above it. This is the only knob that costs CPU.
//  - **Drop the audio track.** No audio track means no audio decoder at
//    playback.
//  - **Disable B-frames.** No reorder buffer at playback, which both shrinks
//    the decoder's pool and removes decode latency.
//  - **moov before mdat.** Opening the file does not read to the end first.
//
// Everything runs through IMFSourceReader → IMFSinkWriter rather than the
// transcode/topology API. The reader/writer pair is the direct analogue of
// AVAssetReader/AVAssetWriter and, unlike a media session, it hands the pump
// over: the frame grid below is enforced by dropping and restamping samples,
// which a topology gives no place to do.
#pragma once

#include <windows.h>

#include <array>
#include <functional>
#include <string>
#include <string_view>

#include "import/ImportOptions.h"

namespace livewall {

class Transcoder {
public:
    // Measured on the macOS side against the real playback path: going from
    // 1280x720 to 3492x1964 (7.4x the pixels) moved the footprint 15 MB → 18 MB
    // and left CPU inside the noise, while going from 24 to 60 fps took CPU
    // from 3.5% to 7.8%. Hardware decode is fixed-function and flat in frame
    // size; the CPU that remains is the app's own per-frame pump work, which
    // scales with frames per second and nothing else.
    //
    // So the presets spend freely on resolution, bit depth and bitrate — all
    // close to free at playback — and stay frugal with frame rate.
    struct Preset {
        const char* name;
        // Longest output edge in pixels, or 0 to size to the display.
        int maxEdge;
        int fps;
        // Bits per pixel per second. Costs disk and nothing else: the 6.8 Mbps
        // variant measured marginally *cheaper* to play than the 1.4 Mbps one,
        // because starving a smooth gradient produces banding the encoder then
        // spends effort on.
        double bitsPerPixel;
        // 10-bit costs nothing to decode and is the real fix for banding. Only
        // honoured when this machine has a Main10 path; see CodecSupport.
        int bitDepth;
        // Keyframes are expensive at these bitrates and a wallpaper seeks only
        // when it resumes from occlusion, so they can be sparse.
        double keyframeSeconds;

        std::string summary() const;
    };

    static const Preset kUltraLight;
    static const Preset kBalanced;
    static const Preset kNative;
    static std::array<const Preset*, 3> allPresets();
    static const Preset* presetByName(std::string_view name);

    // What the output should be sized and paced against. Sourced from the
    // display the wallpaper will actually play on.
    struct DisplayTarget {
        // Physical pixels. Per-monitor-v2 DPI awareness means this is the real
        // panel size rather than a virtualised 96-DPI rectangle.
        int pixelWidth = 1920;
        int pixelHeight = 1080;
        int refreshHz = 60;

        // The monitor the mouse is on, which is the one the user is looking at
        // when they pick Add Video.
        static DisplayTarget primary();
    };

    struct Result {
        std::wstring path;
        int width = 0;
        int height = 0;
        int fps = 0;
        int bitDepth = 8;
        std::string codec;
        long long byteCount = 0;
    };

    // Converts `source` into `destination`. `progress` is called from the
    // calling thread with 0...1; `cancelled` is polled between frames so the
    // caller can abort a long import.
    //
    // Returns an empty error string on success, or a sentence fit to show the
    // user. Media Foundation's own messages are HRESULTs and "The request is
    // invalid in the current state", which tell a user nothing actionable.
    static std::string convert(const std::wstring& source, const std::wstring& destination,
                               const Preset& preset, const DisplayTarget& display,
                               const ImportOptions& options,
                               const std::function<void(double)>& progress,
                               const std::function<bool()>& cancelled, Result* result);

    // Largest frame rate no greater than `preferred` that divides `refresh`
    // exactly. Falls back to `preferred` when nothing sensible divides it.
    //
    // Why it matters: a frame that arrives between two refreshes is decoded and
    // then dropped by the compositor, which is the most wasteful thing this
    // pipeline can do. 24 divides 120 exactly; on a 60 Hz panel it does not,
    // and 20 is both smoother and cheaper.
    static int pacedFPS(int preferred, int refresh);

    // Output dimensions for a preset.
    //
    // A fixed `maxEdge` fits the source inside that edge. Zero sizes to the
    // display instead — scaled so the frame *covers* the panel, since anything
    // less is upscaled at playback and that upscale was the single largest
    // quality loss in the pipeline. Neither path ever scales above 1:1: the
    // source has no detail past its own resolution and inventing pixels only
    // costs memory.
    static void outputSize(int sourceWidth, int sourceHeight, const Preset& preset,
                           const DisplayTarget& display, int* outWidth, int* outHeight);

    // Initialises Media Foundation for the calling thread. Idempotent.
    static bool startupMediaFoundation();
    static void shutdownMediaFoundation();
};

}  // namespace livewall
