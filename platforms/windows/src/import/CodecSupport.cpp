#include "import/CodecSupport.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include "support/Log.h"

namespace livewall {
namespace {

using Microsoft::WRL::ComPtr;

bool anyTransform(const GUID& category, const GUID& inputSubtype, const GUID& outputSubtype,
                  bool inputIsMajorVideo) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, inputSubtype};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, outputSubtype};

    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    // Hardware first, then software. A hardware encoder is the only kind worth
    // having — the software HEVC encoder, where it exists at all, runs at a few
    // frames a second on a 4K source and would make import take minutes.
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT |
                         MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;

    const HRESULT hr = MFTEnumEx(category, flags, inputIsMajorVideo ? &input : nullptr,
                                 &output, &activates, &count);
    if (SUCCEEDED(hr) && activates != nullptr) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i] != nullptr) activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    return SUCCEEDED(hr) && count > 0;
}

bool probeHevcEncoder() {
    // NV12 in, HEVC out. Every hardware encoder takes NV12; asking about a
    // format the encoder does not accept would report "no encoder" on a machine
    // that has a perfectly good one.
    return anyTransform(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_NV12, MFVideoFormat_HEVC,
                        /*inputIsMajorVideo=*/true);
}

bool probeHevcDecoder() {
    return anyTransform(MFT_CATEGORY_VIDEO_DECODER, MFVideoFormat_HEVC, MFVideoFormat_NV12,
                        /*inputIsMajorVideo=*/true);
}

bool probeHevcMain10() {
    // P010 output is what distinguishes a Main10-capable decoder from one that
    // only handles 8-bit. An encoder that will not take P010 in is equally
    // disqualifying, so both ends are checked.
    const bool decodes = anyTransform(MFT_CATEGORY_VIDEO_DECODER, MFVideoFormat_HEVC,
                                      MFVideoFormat_P010, /*inputIsMajorVideo=*/true);
    const bool encodes = anyTransform(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_P010,
                                      MFVideoFormat_HEVC, /*inputIsMajorVideo=*/true);
    return decodes && encodes;
}

struct Probe {
    bool hevcEncoder = false;
    bool hevcDecoder = false;
    bool main10 = false;
};

const Probe& probe() {
    static const Probe result = [] {
        Probe p;
        p.hevcEncoder = probeHevcEncoder();
        p.hevcDecoder = probeHevcDecoder();
        p.main10 = p.hevcEncoder && p.hevcDecoder && probeHevcMain10();

        Log::info(std::string("codec probe — HEVC encoder: ") +
                  (p.hevcEncoder ? "yes" : "no") + ", HEVC decoder: " +
                  (p.hevcDecoder ? "yes" : "no") + ", Main10: " + (p.main10 ? "yes" : "no"));
        return p;
    }();
    return result;
}

}  // namespace

bool CodecSupport::hasHevcEncoder() { return probe().hevcEncoder; }
bool CodecSupport::hasHevcDecoder() { return probe().hevcDecoder; }
bool CodecSupport::hasHevcMain10() { return probe().main10; }

CodecChoice CodecSupport::best(int preferredBitDepth) {
    const Probe& p = probe();

    if (p.hevcEncoder && p.hevcDecoder) {
        if (preferredBitDepth >= 10 && p.main10) {
            return {MFVideoFormat_HEVC, 10, "HEVC 10-bit"};
        }
        return {MFVideoFormat_HEVC, 8, "HEVC 8-bit"};
    }

    // The floor. H.264 High profile, 8-bit — High10 exists on paper and almost
    // nothing decodes it in hardware, which would trade a disk saving for the
    // one cost that actually matters.
    return {MFVideoFormat_H264, 8, "H.264 8-bit"};
}

std::string CodecSupport::fallbackExplanation() {
    const Probe& p = probe();

    if (!p.hevcEncoder && !p.hevcDecoder) {
        return "This machine has no HEVC encoder or decoder, so imports use H.264. "
               "Files are larger; playback costs the same.";
    }
    if (!p.hevcEncoder) {
        return "This machine can play HEVC but not encode it — that comes from the graphics "
               "driver. Imports use H.264 instead.";
    }
    if (!p.hevcDecoder) {
        return "This machine can encode HEVC but has no decoder for it. Install HEVC Video "
               "Extensions from the Microsoft Store, or leave imports on H.264.";
    }
    if (!p.main10) {
        return "HEVC works here, but not 10-bit. Imports use 8-bit, which can show banding "
               "in smooth gradients.";
    }
    return {};
}

}  // namespace livewall
