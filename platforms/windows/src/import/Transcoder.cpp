#include "import/Transcoder.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>

#include "import/CodecSupport.h"
#include "import/FramePacer.h"
#include "import/FrameRotate.h"
#include "import/ImportOptions.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

using Microsoft::WRL::ComPtr;

std::atomic<int> g_mediaFoundationUsers{0};

// Rounds down to an even number. 4:2:0 chroma subsampling requires it, and an
// odd dimension is rejected by every hardware encoder with an unhelpful error.
int even(double value) {
    int rounded = static_cast<int>(value + 0.5);
    rounded -= rounded % 2;
    return std::max(2, rounded);
}

std::string describe(HRESULT hr, const char* stage) {
    // MF_E_TOPO_CODEC_NOT_FOUND and friends are the interesting cases; the rest
    // get the generic wording plus the code, which at least makes a bug report
    // actionable.
    switch (hr) {
        case MF_E_UNSUPPORTED_BYTESTREAM_TYPE:
        case MF_E_UNSUPPORTED_FORMAT:
        case MF_E_INVALIDMEDIATYPE:
            return "That file's format isn't one Windows can read. Try an MP4, MOV or MKV.";
        case MF_E_SOURCERESOLVER_MUTUALLY_EXCLUSIVE_FLAGS:
        case MF_E_CANNOT_PARSE_BYTESTREAM:
            return "That file couldn't be read. It may be damaged or not a video file.";
        case MF_E_TRANSFORM_TYPE_NOT_SET:
        case MF_E_TOPO_CODEC_NOT_FOUND:
            return "No encoder on this machine accepts these settings. Try the Balanced or "
                   "Ultra Light preset.";
        case E_ACCESSDENIED:
            return "Access to that file was denied.";
        default:
            return std::string("Conversion failed at ") + stage + ": " + Log::hresult(hr);
    }
}

// Turns one NV12 or P010 sample into a fresh sample of the rotated geometry.
//
// Both formats are two-plane 4:2:0: a full-resolution luma plane, then a
// half-resolution plane holding interleaved U and V. `lumaBytes` is 1 for NV12
// and 2 for P010, which makes the chroma element twice that in each case — and
// since a chroma row holds `width / 2` elements of `2 * lumaBytes`, both planes
// end up on the same stride, `width * lumaBytes`.
//
// The sample is flattened with ConvertToContiguousBuffer rather than locked as a
// 2D buffer: the rotation reads every byte exactly once regardless, so the copy
// a non-contiguous sample would cost is the copy that would happen anyway.
bool rotateSample(IMFSample* source, int srcWidth, int srcHeight, int dstWidth, int dstHeight,
                  int lumaBytes, int degrees, IMFSample** rotated) {
    ComPtr<IMFMediaBuffer> sourceBuffer;
    if (FAILED(source->ConvertToContiguousBuffer(&sourceBuffer))) return false;

    const DWORD lumaBytesTotal =
        static_cast<DWORD>(srcWidth) * static_cast<DWORD>(srcHeight) * lumaBytes;
    // 4:2:0 chroma is a quarter of the sample positions carrying two components.
    const DWORD frameBytes = lumaBytesTotal + lumaBytesTotal / 2;

    ComPtr<IMFMediaBuffer> destinationBuffer;
    if (FAILED(MFCreateMemoryBuffer(frameBytes, &destinationBuffer))) return false;

    BYTE* sourceData = nullptr;
    DWORD sourceLength = 0;
    if (FAILED(sourceBuffer->Lock(&sourceData, nullptr, &sourceLength))) return false;
    if (sourceLength < frameBytes) {
        sourceBuffer->Unlock();
        return false;
    }

    BYTE* destinationData = nullptr;
    if (FAILED(destinationBuffer->Lock(&destinationData, nullptr, nullptr))) {
        sourceBuffer->Unlock();
        return false;
    }

    const int sourceStride = srcWidth * lumaBytes;
    const int destinationStride = dstWidth * lumaBytes;

    PlaneGeometry luma;
    luma.width = srcWidth;
    luma.height = srcHeight;
    luma.elementBytes = lumaBytes;
    rotatePlane(sourceData, sourceStride, luma, destinationData, destinationStride, degrees);

    PlaneGeometry chroma;
    chroma.width = srcWidth / 2;
    chroma.height = srcHeight / 2;
    chroma.elementBytes = lumaBytes * 2;
    rotatePlane(sourceData + lumaBytesTotal, sourceStride, chroma,
                destinationData + static_cast<DWORD>(dstWidth) * dstHeight * lumaBytes,
                destinationStride, degrees);

    destinationBuffer->Unlock();
    sourceBuffer->Unlock();
    destinationBuffer->SetCurrentLength(frameBytes);

    ComPtr<IMFSample> output;
    if (FAILED(MFCreateSample(&output))) return false;
    if (FAILED(output->AddBuffer(destinationBuffer.Get()))) return false;

    // Timestamps are stamped by the pacer after this returns, but carrying the
    // source's across keeps a sample that is inspected in between coherent.
    LONGLONG time = 0;
    LONGLONG duration = 0;
    if (SUCCEEDED(source->GetSampleTime(&time))) output->SetSampleTime(time);
    if (SUCCEEDED(source->GetSampleDuration(&duration))) output->SetSampleDuration(duration);

    *rotated = output.Detach();
    return true;
}

// Applies the encoder settings that a media type cannot carry. These live on
// the encoder MFT's ICodecAPI, which the sink writer exposes per stream.
void configureEncoder(IMFSinkWriter* writer, DWORD stream, const Transcoder::Preset& preset,
                      int fps) {
    ComPtr<IMFSinkWriterEx> writerEx;
    if (FAILED(writer->QueryInterface(IID_PPV_ARGS(&writerEx)))) return;

    ComPtr<IMFTransform> transform;
    GUID category{};
    // Index 0 is the encoder itself; anything past it is a converter the writer
    // inserted, and those have no settings worth touching.
    if (FAILED(writerEx->GetTransformForStream(stream, 0, &category, &transform))) return;

    ComPtr<ICodecAPI> codec;
    if (FAILED(transform.As(&codec))) return;

    VARIANT value;

    // No B-frames. The decoder then needs no reorder buffer at playback, which
    // both shrinks its pool and removes a frame of decode latency — and the
    // playback path pulls exactly one frame per tick, so latency it cannot
    // absorb is latency that shows.
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = 0;
    codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value);
    VariantClear(&value);

    // Sparse keyframes. At these bitrates an I-frame every two seconds ate a
    // large share of the budget the moving content wanted, and the only seek a
    // wallpaper performs is the resume after occlusion, which tolerates a
    // longer run-up.
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = static_cast<ULONG>(fps * preset.keyframeSeconds);
    codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &value);
    VariantClear(&value);

    // Unconstrained VBR: the content is a loop of smooth gradients whose
    // difficulty varies a great deal across the clip, and a hard CBR ceiling
    // spends the same bits on the flat parts as on the busy ones.
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
    codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &value);
    VariantClear(&value);

    // Quality over speed. This is a one-time import cost, and the file is then
    // played thousands of times.
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = 100;
    codec->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &value);
    VariantClear(&value);

    // Low latency off: it disables lookahead, and lookahead is most of what
    // makes a VBR encode allocate bits sensibly across a loop.
    VariantInit(&value);
    value.vt = VT_BOOL;
    value.boolVal = VARIANT_FALSE;
    codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
    VariantClear(&value);
}

}  // namespace

const Transcoder::Preset Transcoder::kUltraLight{"Ultra Light", 960, 20, 0.10, 8, 5.0};
const Transcoder::Preset Transcoder::kBalanced{"Balanced", 1920, 24, 0.15, 8, 5.0};
// 24 rather than 30: frame rate is the one knob that costs CPU linearly, it
// divides a 120 Hz panel as evenly as 30 does, and ambient loops gain nothing
// from the extra six frames.
const Transcoder::Preset Transcoder::kNative{"Native", 0, 24, 0.12, 10, 5.0};

std::array<const Transcoder::Preset*, 3> Transcoder::allPresets() {
    return {&kUltraLight, &kBalanced, &kNative};
}

const Transcoder::Preset* Transcoder::presetByName(std::string_view name) {
    // "Fidelity" was the old top preset on the macOS side. Anyone who chose it
    // wanted the best available, which is now "Native"; the two apps share an
    // index format, so the old name can arrive here.
    if (name == "Fidelity") return &kNative;
    for (const Preset* preset : allPresets()) {
        if (name == preset->name) return preset;
    }
    return &kBalanced;
}

std::string Transcoder::Preset::summary() const {
    const std::string size = maxEdge > 0 ? std::to_string(maxEdge) + "p" : "your display";
    return format("%s · %d fps · %d-bit", size.c_str(), fps, bitDepth);
}

Transcoder::DisplayTarget Transcoder::DisplayTarget::primary() {
    DisplayTarget target;

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) != 0) {
        target.pixelWidth = info.rcMonitor.right - info.rcMonitor.left;
        target.pixelHeight = info.rcMonitor.bottom - info.rcMonitor.top;

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) != 0 &&
            mode.dmDisplayFrequency > 1) {
            target.refreshHz = static_cast<int>(mode.dmDisplayFrequency);
        }
    }
    return target;
}

bool Transcoder::startupMediaFoundation() {
    if (g_mediaFoundationUsers.fetch_add(1) > 0) return true;

    const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        g_mediaFoundationUsers.fetch_sub(1);
        Log::error("MFStartup failed: " + Log::hresult(hr));
        return false;
    }
    return true;
}

void Transcoder::shutdownMediaFoundation() {
    if (g_mediaFoundationUsers.fetch_sub(1) == 1) MFShutdown();
}

int Transcoder::pacedFPS(int preferred, int refresh) {
    if (preferred <= 0 || refresh <= 0) return std::max(1, preferred);

    int candidate = std::min(preferred, refresh);
    // 12 fps is the floor worth snapping to; below that the cure is worse than
    // the judder.
    while (candidate >= 12) {
        if (refresh % candidate == 0) return candidate;
        --candidate;
    }
    return preferred;
}

void Transcoder::outputSize(int sourceWidth, int sourceHeight, const Preset& preset,
                            const DisplayTarget& display, int* outWidth, int* outHeight) {
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        const int edge = preset.maxEdge > 0 ? preset.maxEdge : display.pixelWidth;
        *outWidth = even(edge);
        *outHeight = even(edge * 9.0 / 16.0);
        return;
    }

    double scale = 1.0;
    if (preset.maxEdge > 0) {
        const int longest = std::max(sourceWidth, sourceHeight);
        scale = longest > preset.maxEdge ? static_cast<double>(preset.maxEdge) / longest : 1.0;
    } else {
        const double cover = std::max(static_cast<double>(display.pixelWidth) / sourceWidth,
                                      static_cast<double>(display.pixelHeight) / sourceHeight);
        // Never above 1:1 — the source has no detail past its own resolution
        // and inventing pixels only costs memory at playback.
        scale = std::min(cover, 1.0);
    }

    *outWidth = even(sourceWidth * scale);
    *outHeight = even(sourceHeight * scale);
}

std::string Transcoder::convert(const std::wstring& source, const std::wstring& destination,
                                const Preset& preset, const DisplayTarget& display,
                                const std::function<void(double)>& progress,
                                const std::function<bool()>& cancelled, Result* result) {
    if (!startupMediaFoundation()) return "Media Foundation is unavailable on this machine.";
    struct Shutdown {
        ~Shutdown() { Transcoder::shutdownMediaFoundation(); }
    } shutdownGuard;

    if (!paths::fileExists(source)) return "That file no longer exists.";

    // ---------------------------------------------------------------------
    // Reader
    // ---------------------------------------------------------------------
    ComPtr<IMFAttributes> readerAttributes;
    if (FAILED(MFCreateAttributes(&readerAttributes, 2))) return "Out of memory.";
    // Advanced video processing ON, unlike the playback path. Here it is
    // exactly what is wanted: it inserts the video processor, which does the
    // downscale and any rotation the source carries, on the GPU, once, at
    // import. The playback path refuses it because there is nothing left to
    // process by then.
    readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    readerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(source.c_str(), readerAttributes.Get(), &reader);
    if (FAILED(hr)) return describe(hr, "opening the file");

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                    TRUE);
    if (FAILED(hr)) return "That file has no video track.";

    ComPtr<IMFMediaType> nativeType;
    hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                    &nativeType);
    if (FAILED(hr)) return "That file has no video track.";

    UINT32 sourceWidth = 0;
    UINT32 sourceHeight = 0;
    MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &sourceWidth, &sourceHeight);

    // Rotation metadata: a phone video is stored landscape with a rotation
    // attribute, and its real dimensions are the transposed ones. The video
    // processor applies the rotation for us, but the *output size* has to be
    // computed against the rotated shape or a portrait clip is squeezed into a
    // landscape frame.
    UINT32 rotation = 0;
    nativeType->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation);
    if (rotation == 90 || rotation == 270) std::swap(sourceWidth, sourceHeight);

    // The user's quarter turn is applied on top of whatever the processor
    // already did, so the two compose: a clip that arrives with a 90 degree flag
    // and is turned another 90 here ends up at 180.
    const int turn = ImportOptions::normalised(options.rotationDegrees);
    if (!options.isQuarterTurn()) {
        return format("%d degrees is not a quarter turn.", options.rotationDegrees);
    }
    const bool rotates = turn != 0;
    if (options.swapsEdges()) std::swap(sourceWidth, sourceHeight);

    UINT32 rateNumerator = 0;
    UINT32 rateDenominator = 0;
    MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE, &rateNumerator, &rateDenominator);
    const int sourceFPS = rateDenominator > 0
                              ? static_cast<int>(static_cast<double>(rateNumerator) /
                                                     rateDenominator + 0.5)
                              : 0;

    int width = 0;
    int height = 0;
    outputSize(static_cast<int>(sourceWidth), static_cast<int>(sourceHeight), preset, display,
               &width, &height);

    // What the video processor is asked to produce, before the user's turn. For
    // a quarter turn that is the output with its edges swapped back — the
    // processor scales, and `rotateSample` below transposes.
    const int scaledWidth = options.swapsEdges() ? height : width;
    const int scaledHeight = options.swapsEdges() ? width : height;

    // Never upsample the frame rate past the source, then land on a rate the
    // display can present evenly. The user's chosen rate is the input to both
    // rules rather than an exemption from either.
    const int preferred = options.preferredFps(preset.fps);
    const int capped = sourceFPS > 0 ? std::min(preferred, sourceFPS) : preferred;
    if (capped != preferred) {
        Log::info(format("capped %d fps to the source's %d", preferred, capped));
    }
    const int fps = std::max(1, pacedFPS(capped, display.refreshHz));
    if (fps != capped) {
        Log::info(format("paced %d fps down to %d to divide a %d Hz display evenly", capped, fps,
                         display.refreshHz));
    }

    const CodecChoice codec = CodecSupport::best(preset.bitDepth);

    // The intermediate the reader is asked to produce. P010 when the encode is
    // 10-bit — encoding to 10 bits is pointless if the video processor has
    // already crushed the frames to 8.
    ComPtr<IMFMediaType> readerOutput;
    if (FAILED(MFCreateMediaType(&readerOutput))) return "Out of memory.";
    readerOutput->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    readerOutput->SetGUID(MF_MT_SUBTYPE,
                          codec.bitDepth >= 10 ? MFVideoFormat_P010 : MFVideoFormat_NV12);
    MFSetAttributeSize(readerOutput.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(scaledWidth),
                       static_cast<UINT32>(scaledHeight));
    MFSetAttributeRatio(readerOutput.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    readerOutput->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                     nullptr, readerOutput.Get());
    if (FAILED(hr) && codec.bitDepth >= 10) {
        Log::info("the video processor refused a P010 intermediate; using NV12");
        readerOutput->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        hr = reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, readerOutput.Get());
    }
    if (FAILED(hr)) return describe(hr, "setting up the scaler");

    // Which intermediate the processor actually agreed to, since the P010
    // request above may have fallen back. P010 stores each component in two
    // bytes; NV12 in one. The rotation needs the real answer, not the request.
    GUID intermediate = GUID_NULL;
    readerOutput->GetGUID(MF_MT_SUBTYPE, &intermediate);
    const int lumaBytes = intermediate == MFVideoFormat_P010 ? 2 : 1;

    // Total duration, for progress.
    long long durationHns = 0;
    PROPVARIANT durationVariant;
    PropVariantInit(&durationVariant);
    if (SUCCEEDED(reader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
            &durationVariant)) &&
        durationVariant.vt == VT_UI8) {
        durationHns = static_cast<long long>(durationVariant.uhVal.QuadPart);
    }
    PropVariantClear(&durationVariant);

    // ---------------------------------------------------------------------
    // Writer
    // ---------------------------------------------------------------------
    paths::removeFile(destination);

    ComPtr<IMFAttributes> writerAttributes;
    if (FAILED(MFCreateAttributes(&writerAttributes, 3))) return "Out of memory.";
    writerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    // Throttling exists to keep a real-time capture in step with the clock.
    // This is an offline convert and there is nothing to stay in step with;
    // leaving it on makes the import take as long as the video.
    writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    // The `shouldOptimizeForNetworkUse` analogue: the moov atom goes at the
    // front, so opening the file does not read to the end first. On a wallpaper
    // that is reopened every time the desktop becomes visible, that is the
    // difference between an instant resume and a full-file read.
    writerAttributes->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT, TRUE);

    ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(destination.c_str(), nullptr, writerAttributes.Get(), &writer);
    if (FAILED(hr)) return describe(hr, "creating the output file");

    const double pixels = static_cast<double>(width) * height;
    const int bitrate = static_cast<int>(pixels * fps * preset.bitsPerPixel);

    ComPtr<IMFMediaType> encodeType;
    if (FAILED(MFCreateMediaType(&encodeType))) return "Out of memory.";
    encodeType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    encodeType->SetGUID(MF_MT_SUBTYPE, codec.subtype);
    encodeType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));
    encodeType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(encodeType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                       static_cast<UINT32>(height));
    MFSetAttributeRatio(encodeType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(encodeType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    if (IsEqualGUID(codec.subtype, MFVideoFormat_HEVC)) {
        // Main (1) or Main10 (2).
        encodeType->SetUINT32(MF_MT_MPEG2_PROFILE,
                              codec.bitDepth >= 10 ? 2u : 1u);
    } else {
        encodeType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    }

    DWORD streamIndex = 0;
    hr = writer->AddStream(encodeType.Get(), &streamIndex);
    if (FAILED(hr)) return describe(hr, "configuring the encoder");

    // The reader's output type describes the frames going in.
    ComPtr<IMFMediaType> inputType;
    hr = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                     &inputType);
    if (FAILED(hr)) return describe(hr, "reading the scaler's output format");

    hr = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr);
    if (FAILED(hr)) return describe(hr, "connecting the scaler to the encoder");

    hr = writer->BeginWriting();
    if (FAILED(hr)) return describe(hr, "starting the encode");

    configureEncoder(writer.Get(), streamIndex, preset, fps);

    // ---------------------------------------------------------------------
    // Pump
    //
    // The video processor rescales frames but does not retime them — it still
    // emits at the source cadence. The frame grid has to be enforced here, by
    // dropping source frames that fall between grid points and stamping the
    // survivors onto exact 1/fps boundaries.
    // ---------------------------------------------------------------------
    FramePacer pacer(fps);
    bool aborted = false;
    std::string failure;

    for (;;) {
        if (cancelled && cancelled()) {
            aborted = true;
            break;
        }

        DWORD stream = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;

        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                &stream, &flags, &timestamp, &sample);
        if (FAILED(hr)) {
            failure = describe(hr, "reading a frame");
            break;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
        if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            failure = "The file ended unexpectedly. It may be damaged.";
            break;
        }
        if (!sample) continue;

        const FramePacer::Decision decision = pacer.accept(timestamp);
        if (decision.keep) {
            // Rotate before stamping, so the timestamps the encoder sees belong
            // to the sample it is actually given.
            ComPtr<IMFSample> outgoing = sample;
            if (rotates) {
                ComPtr<IMFSample> turned;
                if (!rotateSample(sample.Get(), scaledWidth, scaledHeight, width, height,
                                  lumaBytes, turn, &turned)) {
                    failure = "Could not rotate a frame.";
                    break;
                }
                outgoing = turned;
            }

            outgoing->SetSampleTime(decision.outputTimeHns);
            outgoing->SetSampleDuration(decision.durationHns);

            hr = writer->WriteSample(streamIndex, outgoing.Get());
            if (FAILED(hr)) {
                failure = describe(hr, "encoding a frame");
                break;
            }
        }

        if (progress && durationHns > 0) {
            progress(std::clamp(static_cast<double>(timestamp) / durationHns, 0.0, 1.0));
        }
    }

    if (aborted) {
        writer->Finalize();
        paths::removeFile(destination);
        return "Conversion cancelled.";
    }
    if (!failure.empty()) {
        writer->Finalize();
        paths::removeFile(destination);
        return failure;
    }

    hr = writer->Finalize();
    if (FAILED(hr)) {
        paths::removeFile(destination);
        return describe(hr, "finishing the file");
    }

    if (pacer.kept() == 0) {
        paths::removeFile(destination);
        return "No frames could be read from that file.";
    }

    if (result != nullptr) {
        result->path = destination;
        result->width = width;
        result->height = height;
        result->fps = fps;
        result->bitDepth = codec.bitDepth;
        result->codec = codec.name;
        result->byteCount = paths::fileSize(destination);
    }

    if (progress) progress(1.0);
    Log::info(format("imported %dx%d @ %d fps, %s, kept %d frames and dropped %d", width, height,
                     fps, codec.name.c_str(), pacer.kept(), pacer.dropped()));
    return {};
}

}  // namespace livewall
