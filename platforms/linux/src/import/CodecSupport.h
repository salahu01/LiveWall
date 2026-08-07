// Which encoder this machine actually has.
//
// The other two ports do not need this file. AVFoundation and Media Foundation
// both guarantee an HEVC encoder, so their transcoders name it and move on. A
// Linux distribution's FFmpeg build may have libx265, or only libx264, or
// neither and a VA-API device instead — Debian and Ubuntu ship an LGPL build
// without x264/x265 in some configurations, and a stripped container image has
// no /dev/dri at all.
//
// So the encoder is *chosen* here, at import time, and the codec that was used
// is written into the library index. The playback path reads it back rather
// than assuming HEVC, which is the one place the Linux index differs from the
// other two ports' — and the field is optional, so an index written by any of
// the three still decodes everywhere.
//
// The order below is by playback cost, not by encode quality or speed. What
// this app optimises is the cost of *playing* the result on the machine that
// imported it, forever, and a one-off import that takes twice as long to
// produce a file the GPU can decode in fixed-function hardware is a trade worth
// making every time.
#pragma once

#include <optional>
#include <string>

namespace livewall {

struct EncoderChoice {
    // The FFmpeg encoder name, e.g. "libx265" or "hevc_vaapi".
    std::string name;
    // What goes in the index and what VideoSource reports: "hevc" or "h264".
    std::string codec;
    // True when the encoder needs frames uploaded into a hardware frames
    // context rather than handed over in system memory.
    bool hardware = false;
    // The highest bit depth this encoder will accept here. 8 for everything
    // except HEVC.
    int maxBitDepth = 8;
    // The pixel format frames must be in. For a hardware encoder this is the
    // *software* format of the frames context, not AV_PIX_FMT_VAAPI.
    int pixelFormat = 0;
};

class CodecSupport {
public:
    // Best available encoder for the requested depth, or nothing when the
    // FFmpeg build has no usable video encoder at all — which means imports are
    // impossible and is reported as such rather than failing per file.
    static std::optional<EncoderChoice> chooseEncoder(int wantBitDepth);

    // One line for `livewall status`, listing what was found.
    static std::string summary();
};

}  // namespace livewall
