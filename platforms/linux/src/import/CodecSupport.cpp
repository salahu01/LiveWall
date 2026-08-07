#include "import/CodecSupport.h"

#include <unistd.h>
#include <vector>

#include "import/FFmpeg.h"
#include "support/Log.h"

namespace livewall {
namespace {

struct Candidate {
    const char* encoder;
    const char* codec;
    bool hardware;
    int maxBitDepth;
    AVPixelFormat eightBit;
    AVPixelFormat tenBit;
};

// Ordered by what the result costs to play, then by how widely the encoder is
// available.
//
// HEVC above H.264 because HEVC decode is fixed-function on every GPU that has
// a video engine at all, and because it is what the other two ports produce —
// keeping the library format the same across platforms is worth something.
//
// Hardware encoders below the software ones despite being faster: an import
// happens once and playback happens for months, and libx265 at a slow preset
// produces a materially smaller file at the same quality than hevc_vaapi does.
// The VA-API entries are there for the LGPL FFmpeg builds that have no x265 at
// all, which is the case this list exists to handle.
constexpr Candidate kCandidates[] = {
    {"libx265", "hevc", false, 10, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10LE},
    {"libx264", "h264", false, 8, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P},
    {"hevc_vaapi", "hevc", true, 10, AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE},
    {"h264_vaapi", "h264", true, 8, AV_PIX_FMT_NV12, AV_PIX_FMT_NV12},
    {"hevc_nvenc", "hevc", true, 10, AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE},
    {"h264_nvenc", "h264", true, 8, AV_PIX_FMT_NV12, AV_PIX_FMT_NV12},
    // Last resort. Present in every FFmpeg build, including the most minimal
    // LGPL one, so it is what stands between "imports produce a large file" and
    // "imports do not work". Decoded in fixed-function hardware on every GPU
    // made since about 2008, which is the property that matters at playback.
    {"mpeg4", "mpeg4", false, 8, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P},
};

// A hardware encoder that FFmpeg was compiled with is still useless without a
// render node to run it on, and `avcodec_find_encoder_by_name` cannot tell.
bool haveRenderNode() {
    for (const char* node : {"/dev/dri/renderD128", "/dev/dri/renderD129"}) {
        if (::access(node, R_OK | W_OK) == 0) return true;
    }
    return false;
}

}  // namespace

std::optional<EncoderChoice> CodecSupport::chooseEncoder(int wantBitDepth) {
    if (!ffmpeg::load()) return std::nullopt;
    const ffmpeg::Api& av = ffmpeg::api();

    const bool renderNode = haveRenderNode();

    // Two passes. The first insists on an encoder that can do the depth that
    // was asked for; the second takes anything. Without the split, a request
    // for 10-bit on a machine with only libx264 would fall to the second entry
    // and silently produce 8-bit when a lower-priority 10-bit encoder was
    // available further down.
    for (const bool insistOnDepth : {true, false}) {
        for (const Candidate& candidate : kCandidates) {
            if (candidate.hardware && !renderNode) continue;
            if (insistOnDepth && wantBitDepth > candidate.maxBitDepth) continue;
            if (av.find_encoder_by_name(candidate.encoder) == nullptr) continue;

            EncoderChoice choice;
            choice.name = candidate.encoder;
            choice.codec = candidate.codec;
            choice.hardware = candidate.hardware;
            choice.maxBitDepth = candidate.maxBitDepth;

            const bool tenBit = wantBitDepth >= 10 && candidate.maxBitDepth >= 10;
            choice.pixelFormat = tenBit ? candidate.tenBit : candidate.eightBit;
            choice.maxBitDepth = tenBit ? 10 : 8;
            return choice;
        }
    }

    Log::error("this FFmpeg build has no video encoder LiveWall can use");
    return std::nullopt;
}

std::string CodecSupport::summary() {
    if (!ffmpeg::load()) return "no FFmpeg — imports unavailable";
    const ffmpeg::Api& av = ffmpeg::api();

    const bool renderNode = haveRenderNode();
    std::string found;
    for (const Candidate& candidate : kCandidates) {
        if (candidate.hardware && !renderNode) continue;
        if (av.find_encoder_by_name(candidate.encoder) == nullptr) continue;
        if (!found.empty()) found += ", ";
        found += candidate.encoder;
    }

    if (found.empty()) return "no usable encoder";
    return found + (renderNode ? "" : " (no render node, so no hardware encoders)");
}

}  // namespace livewall
