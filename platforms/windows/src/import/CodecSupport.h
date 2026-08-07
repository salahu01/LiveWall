// What this machine can actually encode and decode.
//
// This file has no macOS counterpart, and the difference is the single biggest
// portability problem in the whole project.
//
// On macOS, HEVC is guaranteed. Every Mac that runs the app has a hardware HEVC
// encoder and decoder on the media engine, so `Transcoder` can simply demand
// HEVC Main10 and be right every time.
//
// On Windows none of that holds:
//
//  - The *encoder* comes from the graphics driver. Intel Quick Sync, NVIDIA
//    NVENC and AMD VCE all supply one, but a machine on a basic display driver,
//    a virtual machine, or a remote desktop session has none.
//  - The *decoder* is worse. Microsoft's HEVC decoder is not in a stock Windows
//    install — it ships as the paid "HEVC Video Extensions" from the Store, or
//    as a device-manufacturer variant preinstalled by some OEMs. Hardware HEVC
//    decode through the driver's own MFT usually works without it, but "usually"
//    is not something to build a mandatory import path on.
//
// Writing a file the machine cannot play back is the one failure this app must
// not have: import is mandatory, so an unplayable output means a wallpaper that
// silently never appears. So both halves are probed before choosing, and the
// answer falls back along a ladder:
//
//     HEVC Main10 (10-bit)  →  HEVC Main (8-bit)  →  H.264 High (8-bit)
//
// H.264 is the floor, and it is a safe floor: every Windows install since 7 has
// a decoder, and every GPU of the last decade decodes it in fixed function.
// What it costs is roughly 40% more bitrate for the same quality, which is disk
// space and nothing else — the playback numbers are set by frame rate, not by
// bits (see the macOS README's measurements).
#pragma once

#include <guiddef.h>

#include <string>

namespace livewall {

struct CodecChoice {
    // MFVideoFormat_HEVC or MFVideoFormat_H264.
    GUID subtype{};
    int bitDepth = 8;
    // For the menu and the log: "HEVC 10-bit", "H.264 8-bit".
    std::string name;
};

class CodecSupport {
public:
    // The best codec this machine can both encode and decode, capped at
    // `preferredBitDepth`. Probed once and cached — enumerating MFTs walks the
    // registry and instantiates candidates, which is not something to do per
    // import.
    static CodecChoice best(int preferredBitDepth);

    // Individually testable pieces, exposed because the tray menu tells the
    // user which of the two is missing when it has to fall back.
    static bool hasHevcEncoder();
    static bool hasHevcDecoder();
    static bool hasHevcMain10();

    // One sentence for the menu tooltip, or empty when nothing was given up.
    static std::string fallbackExplanation();
};

}  // namespace livewall
