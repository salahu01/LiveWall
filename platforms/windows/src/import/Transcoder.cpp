#include "import/Transcoder.h"

// codecapi.h defines the CODECAPI_* property GUIDs; the ICodecAPI interface
// they are passed to is declared separately in icodecapi.h. Including only the
// first compiles the GUID references and then fails on the interface itself.
#include <codecapi.h>
#include <icodecapi.h>
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
    // Logged unconditionally, stage and code included. The user-facing text is
    // deliberately plainer than this, but the recognised cases used to return a
    // sentence with neither — "Access to that file was denied." named no stage,
    // carried no code and was never written anywhere — which left no way to tell
    // a source that could not be read from an output that could not be created.
    Log::error(std::string("import failed while ") + stage + ": " + Log::hresult(hr));

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
            return std::string("Windows denied access while ") + stage + ".";
        default:
            return std::string("Conversion failed at ") + stage + ": " + Log::hresult(hr);
    }
}

// Whether the file is a cloud placeholder — a stub whose bytes still live on the
// provider's server. OneDrive's Files On-Demand leaves these behind by default,
// and opening one is a request for the provider to fetch it. A provider that is
// paused, signed out, metered or simply offline refuses, and the refusal that
// reaches the caller is a bare ERROR_ACCESS_DENIED with nothing to say it was
// about the network rather than about permissions.
bool isCloudPlaceholder(DWORD attributes) {
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    return (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                          FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0;
}

// Opens the source the way the reader is about to, so that a refusal can name
// what was refused. MFCreateSourceReaderFromURL collapses locked, unreadable and
// never-downloaded into one E_ACCESSDENIED; Win32 distinguishes them.
std::string checkReadable(const std::wstring& source) {
    const bool placeholder = isCloudPlaceholder(GetFileAttributesW(source.c_str()));

    // Shared every way: this is a read-only probe, and refusing to import a file
    // merely because some other program has it open would be its own bug.
    const HANDLE file =
        CreateFileW(source.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        return {};
    }

    const DWORD error = GetLastError();
    Log::error("cannot open the source file: " + Log::lastError());

    switch (error) {
        case ERROR_ACCESS_DENIED:
            if (placeholder) {
                return "That video is stored online only and Windows could not download it. "
                       "Open it once in File Explorer so your cloud provider fetches a local "
                       "copy, then add it again.";
            }
            return "Windows would not let LiveWall read that file. It may sit in a protected "
                   "folder, or a security product may be blocking it — try copying the video "
                   "somewhere else and adding that copy.";
        case ERROR_SHARING_VIOLATION:
            return "Another program has that file open exclusively. Close it and try again.";
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return "That file no longer exists.";
        default:
            return "That file could not be opened: " + Log::hresult(HRESULT_FROM_WIN32(error));
    }
}

// The same check for the other end. The library folder is under %LOCALAPPDATA%,
// which is writable by definition — right up until a ransomware-protection rule
// or a roaming-profile policy says otherwise, at which point the sink writer
// fails with the identical E_ACCESSDENIED the reader would have produced.
std::string checkWritable(const std::wstring& destination) {
    const size_t slash = destination.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        const std::wstring directory = destination.substr(0, slash);
        // SHCreateDirectoryExW returns its error rather than setting the thread's
        // last-error, so there is nothing more specific to log than the path.
        if (!paths::createDirectories(directory)) {
            Log::error("cannot create the library folder: " + narrow(directory));
            return "LiveWall's library folder could not be created at " + narrow(directory) + ".";
        }
    }

    // Deleted on close, so the probe leaves nothing behind even if the encode
    // never starts.
    const HANDLE probe = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        return {};
    }

    const DWORD error = GetLastError();
    Log::error("cannot write to the library folder: " + Log::lastError());
    if (error == ERROR_ACCESS_DENIED) {
        return "Windows would not let LiveWall write to its own library folder. Controlled "
               "folder access or an antivirus rule is the usual cause — allow LiveWall.exe, "
               "then try again.";
    }
    return "The converted file could not be created: " + Log::hresult(HRESULT_FROM_WIN32(error));
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

    // Both ends are probed here, before any Media Foundation object exists.
    // Doing it afterwards is what produced the unactionable dialog: whichever
    // end was at fault, the failure arrived as the same code from an MF call
    // that would not say which file it had been holding.
    if (const std::string denied = checkReadable(source); !denied.empty()) return denied;
    if (const std::string denied = checkWritable(destination); !denied.empty()) return denied;

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

    // Never upsample the frame rate past the source, then land on a rate the
    // display can present evenly.
    const int capped = sourceFPS > 0 ? std::min(preset.fps, sourceFPS) : preset.fps;
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
    MFSetAttributeSize(readerOutput.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                       static_cast<UINT32>(height));
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

    // The reader's output type describes the frames going in.
    ComPtr<IMFMediaType> inputType;
    hr = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                     &inputType);
    if (FAILED(hr)) return describe(hr, "reading the scaler's output format");

    // ---------------------------------------------------------------------
    // Writer + pump, wrapped in a retryable attempt.
    //
    // MF_MPEG4SINK_MOOV_BEFORE_MDAT is a request, not a guarantee: on some
    // machines the MP4 sink's moov-before-mdat pass fails at Finalize() with
    // E_ACCESSDENIED even though every WriteSample succeeded, because the
    // rewrite it does there needs something the driver's sink implementation
    // won't give it. When that happens the whole encode is retried once with
    // the flag off, trading the fast-resume optimisation for a file that
    // imports at all.
    // ---------------------------------------------------------------------
    struct Attempt {
        std::string failure;
        bool failedAtFinalize = false;
        bool aborted = false;
        int kept = 0;
        int dropped = 0;
    };

    auto attempt = [&](bool moovBeforeMdat) -> Attempt {
        Attempt outcome;
        paths::removeFile(destination);

        ComPtr<IMFAttributes> writerAttributes;
        if (FAILED(MFCreateAttributes(&writerAttributes, 3))) {
            outcome.failure = "Out of memory.";
            return outcome;
        }
        writerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        // Throttling exists to keep a real-time capture in step with the clock.
        // This is an offline convert and there is nothing to stay in step
        // with; leaving it on makes the import take as long as the video.
        writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        // The `shouldOptimizeForNetworkUse` analogue: the moov atom goes at
        // the front, so opening the file does not read to the end first. On a
        // wallpaper that is reopened every time the desktop becomes visible,
        // that is the difference between an instant resume and a full-file
        // read.
        writerAttributes->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT,
                                    moovBeforeMdat ? TRUE : FALSE);

        ComPtr<IMFSinkWriter> writer;
        HRESULT localHr = MFCreateSinkWriterFromURL(destination.c_str(), nullptr,
                                                     writerAttributes.Get(), &writer);
        if (FAILED(localHr)) {
            outcome.failure = describe(localHr, "creating the output file");
            return outcome;
        }

        DWORD streamIndex = 0;
        localHr = writer->AddStream(encodeType.Get(), &streamIndex);
        if (FAILED(localHr)) {
            outcome.failure = describe(localHr, "configuring the encoder");
            return outcome;
        }

        localHr = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr);
        if (FAILED(localHr)) {
            outcome.failure = describe(localHr, "connecting the scaler to the encoder");
            return outcome;
        }

        localHr = writer->BeginWriting();
        if (FAILED(localHr)) {
            outcome.failure = describe(localHr, "starting the encode");
            return outcome;
        }

        configureEncoder(writer.Get(), streamIndex, preset, fps);

        // The video processor rescales frames but does not retime them — it
        // still emits at the source cadence. The frame grid has to be
        // enforced here, by dropping source frames that fall between grid
        // points and stamping the survivors onto exact 1/fps boundaries.
        FramePacer pacer(fps);

        for (;;) {
            if (cancelled && cancelled()) {
                outcome.aborted = true;
                break;
            }

            DWORD stream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;

            localHr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                         0, &stream, &flags, &timestamp, &sample);
            if (FAILED(localHr)) {
                outcome.failure = describe(localHr, "reading a frame");
                break;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                outcome.failure = "The file ended unexpectedly. It may be damaged.";
                break;
            }
            if (!sample) continue;

            const FramePacer::Decision decision = pacer.accept(timestamp);
            if (decision.keep) {
                sample->SetSampleTime(decision.outputTimeHns);
                sample->SetSampleDuration(decision.durationHns);

                localHr = writer->WriteSample(streamIndex, sample.Get());
                if (FAILED(localHr)) {
                    outcome.failure = describe(localHr, "encoding a frame");
                    break;
                }
            }

            if (progress && durationHns > 0) {
                progress(std::clamp(static_cast<double>(timestamp) / durationHns, 0.0, 1.0));
            }
        }

        outcome.kept = pacer.kept();
        outcome.dropped = pacer.dropped();

        if (outcome.aborted || !outcome.failure.empty()) {
            writer->Finalize();
            paths::removeFile(destination);
            return outcome;
        }

        localHr = writer->Finalize();
        if (FAILED(localHr)) {
            paths::removeFile(destination);
            outcome.failure = describe(localHr, "finishing the file");
            outcome.failedAtFinalize = true;
        }
        return outcome;
    };

    Attempt outcome = attempt(true);
    if (outcome.failedAtFinalize) {
        Log::info("moov-before-mdat failed at Finalize; retrying the import without it");

        PROPVARIANT start;
        PropVariantInit(&start);
        start.vt = VT_I8;
        start.hVal.QuadPart = 0;
        reader->SetCurrentPosition(GUID_NULL, start);
        PropVariantClear(&start);

        outcome = attempt(false);
    }

    if (outcome.aborted) return "Conversion cancelled.";
    if (!outcome.failure.empty()) return outcome.failure;

    if (outcome.kept == 0) {
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
                     fps, codec.name.c_str(), outcome.kept, outcome.dropped));
    return {};
}

}  // namespace livewall
