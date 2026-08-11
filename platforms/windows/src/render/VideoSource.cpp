#include "render/VideoSource.h"

#include <mferror.h>
#include <objbase.h>

#include <chrono>
#include <iterator>

#include "support/Footprint.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

// Media Foundation's time unit: 100 ns ticks.
constexpr long long kHnsPerSecond = 10'000'000LL;

}  // namespace

VideoSource::VideoSource(std::shared_ptr<D3DDevice> device, std::wstring path, int fps,
                         int bitDepth, FitMode fitMode)
    : device_(std::move(device)),
      path_(std::move(path)),
      fps_(fps > 0 ? fps : 24),
      bitDepth_(bitDepth >= 10 ? 10 : 8) {
    fitMode_ = static_cast<int>(fitMode);
    stopEvent_ = CreateEventW(nullptr, /*manualReset=*/TRUE, /*initial=*/FALSE, nullptr);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (timer_ == nullptr) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
}

VideoSource::~VideoSource() {
    deactivate();
    if (timer_ != nullptr) CloseHandle(timer_);
    if (stopEvent_ != nullptr) CloseHandle(stopEvent_);
}

std::string VideoSource::summary() const { return narrow(paths::stem(path_)); }

void VideoSource::setFitMode(FitMode mode) {
    // Read by the render thread on the next frame. No decoder or reader
    // involvement, so switching modes never interrupts playback.
    fitMode_ = static_cast<int>(mode);
}

bool VideoSource::prepare() {
    if (!paths::fileExists(path_)) {
        Log::error("wallpaper file is missing: " + narrow(path_));
        return false;
    }
    if (!device_) return false;

    // The DXGI device manager is what makes the decoder hand back textures on
    // our device rather than system-memory buffers it copied out of video
    // memory. Without it every frame is a full read-back across the bus, which
    // at 4K is roughly 12 MB a frame of pure waste.
    HRESULT hr = MFCreateDXGIDeviceManager(&deviceManagerToken_, &deviceManager_);
    if (FAILED(hr)) {
        Log::error("MFCreateDXGIDeviceManager failed: " + Log::hresult(hr));
        return false;
    }
    hr = deviceManager_->ResetDevice(device_->device(), deviceManagerToken_);
    if (FAILED(hr)) {
        Log::error("could not bind the D3D device to Media Foundation: " + Log::hresult(hr));
        return false;
    }

    // A throwaway reader, opened once, purely to read the track's real
    // properties. Building it here rather than at activate time means a file
    // that cannot be decoded is discovered before it is installed, and the
    // caller can fall back to the procedural mode.
    if (!createReader(0)) return false;

    ComPtr<IMFMediaType> type;
    {
        std::lock_guard<std::mutex> guard(readerMutex_);
        if (!reader_) return false;
        hr = reader_->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
    }
    if (FAILED(hr) || !type) {
        Log::error("could not read the video stream's media type: " + Log::hresult(hr));
        releaseReader();
        return false;
    }

    UINT32 width = 0;
    UINT32 height = 0;
    if (SUCCEEDED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height))) {
        frameWidth_ = static_cast<int>(width);
        frameHeight_ = static_cast<int>(height);
    }

    UINT32 numerator = 0;
    UINT32 denominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) &&
        denominator > 0) {
        const int actual = static_cast<int>(
            (static_cast<double>(numerator) / denominator) + 0.5);
        if (actual > 0 && actual != fps_) {
            Log::info(format("track is %d fps, index said %d — using the track", actual, fps_));
            fps_ = actual;
        }
    }

    // Duration, so the loop knows where the end is without waiting for a read
    // to report it.
    PROPVARIANT duration;
    PropVariantInit(&duration);
    {
        std::lock_guard<std::mutex> guard(readerMutex_);
        if (reader_ && SUCCEEDED(reader_->GetPresentationAttribute(
                           static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                           MF_PD_DURATION, &duration)) &&
            duration.vt == VT_UI8) {
            durationHns_ = static_cast<long long>(duration.uhVal.QuadPart);
        }
    }
    PropVariantClear(&duration);

    // The reader is not kept: activate() builds a fresh one, and holding a
    // decoder open across a session where the desktop is covered from the start
    // would be exactly the cost this app exists to avoid.
    releaseReader();

    Log::info(format("prepared %s: %dx%d @ %d fps, %d-bit", summary().c_str(), frameWidth_,
                     frameHeight_, fps_, bitDepth_));
    return frameWidth_ > 0 && frameHeight_ > 0;
}

bool VideoSource::createReader(long long startTimeHns) {
    std::lock_guard<std::mutex> guard(readerMutex_);

    reader_.Reset();

    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 4);
    if (FAILED(hr)) return false;

    attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, deviceManager_.Get());
    // Hardware transforms, which for a video stream means the DXVA decoder MFT
    // the graphics driver supplies rather than the software fallback.
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE);
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    // Without this the reader will not hand back a sample whose buffer is a
    // D3D texture, which defeats the point of the device manager above.
    attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);

    hr = MFCreateSourceReaderFromURL(path_.c_str(), attributes.Get(), &reader_);
    if (FAILED(hr)) {
        Log::error("could not open " + narrow(path_) + ": " + Log::hresult(hr));
        return false;
    }

    // Audio is stripped at import, but a file that arrived some other way might
    // still have a track, and a selected audio stream means the reader spins up
    // an audio decoder for samples nobody reads.
    reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

    // Ask for exactly what the decoder already produces. NV12 for 8-bit, P010
    // for 10-bit: both are the DXVA decoder's native output, so no video
    // processor is inserted and nothing is converted on the way out. Asking for
    // RGB32 here would work and would cost a second full-resolution surface per
    // frame plus the conversion.
    ComPtr<IMFMediaType> desired;
    hr = MFCreateMediaType(&desired);
    if (FAILED(hr)) return false;
    desired->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    desired->SetGUID(MF_MT_SUBTYPE, bitDepth_ >= 10 ? MFVideoFormat_P010 : MFVideoFormat_NV12);

    hr = reader_->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, desired.Get());
    if (FAILED(hr) && bitDepth_ >= 10) {
        // A 10-bit file on a decoder that only offers NV12 output means the
        // driver is down-converting; take it rather than refusing to play.
        Log::info("P010 output was refused; falling back to NV12");
        bitDepth_ = 8;
        desired->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        hr = reader_->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, desired.Get());
    }
    if (FAILED(hr)) {
        Log::error("no decoder on this machine produces NV12 for this file: " + Log::hresult(hr));
        reader_.Reset();
        return false;
    }

    if (startTimeHns > 0 && (durationHns_ == 0 || startTimeHns < durationHns_)) {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = startTimeHns;
        reader_->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
    }

    return true;
}

void VideoSource::releaseReader() {
    std::lock_guard<std::mutex> guard(readerMutex_);
    // This is the difference between "paused" and "costs nothing": releasing
    // the reader releases the decoder MFT, its DXVA decode session and the
    // texture array it allocated for output.
    reader_.Reset();
    lumaView_.Reset();
    chromaView_.Reset();
    shaderTexture_.Reset();
}

void VideoSource::attach(SwapChainTarget* target, int refreshHz) {
    const bool wasActive = active_;
    if (wasActive) deactivate();

    target_ = target;
    refreshHz_ = refreshHz > 0 ? refreshHz : 60;

    // Let DXGI pace when the frame rate divides the refresh rate exactly: a
    // sync interval of refresh/fps blocks Present in the driver until the right
    // vertical blank, which costs no timer, no wake-up and no drift. The
    // transcoder already snaps every imported file's frame rate to a divisor of
    // the display it was imported on, so this is the common case — but a file
    // imported on a 120 Hz laptop and later shown on a 60 Hz monitor is not,
    // and there the timer path takes over.
    syncInterval_ = (fps_ > 0 && refreshHz_ % fps_ == 0)
                        ? static_cast<UINT>(refreshHz_ / fps_)
                        : 0u;

    if (wasActive) activate();
}

void VideoSource::activate() {
    if (active_ || target_ == nullptr || !device_) return;

    if (!createReader(resumeTimeHns_)) {
        Log::error("could not build a decoder for " + summary());
        return;
    }

    stopping_ = false;
    if (stopEvent_ != nullptr) ResetEvent(stopEvent_);
    active_ = true;
    thread_ = std::thread([this] { renderLoop(); });

    Log::info(format("video activated at %.1fs — %s",
                     static_cast<double>(resumeTimeHns_) / kHnsPerSecond,
                     Footprint::formatted().c_str()));
}

void VideoSource::deactivate() {
    if (!active_) return;

    stopping_ = true;
    active_ = false;
    if (stopEvent_ != nullptr) SetEvent(stopEvent_);
    if (timer_ != nullptr) CancelWaitableTimer(timer_);
    if (thread_.joinable()) thread_.join();

    releaseReader();

    // Deliberately no clear and no final present — the last enqueued frame
    // stays in the front buffer so reactivation is seamless.
    Log::info("video deactivated — " + Footprint::formatted());
}

void VideoSource::renderLoop() {
    LARGE_INTEGER period{};
    period.QuadPart = -(kHnsPerSecond / (fps_ > 0 ? fps_ : 24));

    while (!stopping_) {
        // Two pacing paths, and only one of them is ever active:
        //
        //  - syncInterval_ != 0: Present blocks in the driver for the right
        //    number of vertical blanks. No timer, no wake-up, no drift. This
        //    is the good case and the transcoder arranges for it.
        //  - syncInterval_ == 0: a high-resolution waitable timer at 1/fps.
        if (syncInterval_ == 0 && timer_ != nullptr && stopEvent_ != nullptr) {
            SetWaitableTimer(timer_, &period, 0, nullptr, nullptr, FALSE);
            const HANDLE waits[2] = {stopEvent_, timer_};
            if (WaitForMultipleObjects(2, waits, /*waitAll=*/FALSE, INFINITE) == WAIT_OBJECT_0) {
                break;
            }
            if (stopping_) break;
        }

        if (!decodeAndDrawOneFrame()) break;
    }
}

bool VideoSource::decodeAndDrawOneFrame() {
    // Blocks until the previously presented frame has been consumed. With a
    // maximum frame latency of 1 this is what holds the app to one frame in
    // flight, without a queue of its own to size or drain.
    target_->waitForPresentable();
    if (stopping_) return false;

    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;

    {
        std::lock_guard<std::mutex> guard(readerMutex_);
        if (!reader_) return false;

        const HRESULT hr = reader_->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &streamIndex, &flags,
            &timestamp, &sample);
        if (FAILED(hr)) {
            Log::error("ReadSample failed: " + Log::hresult(hr));
            return false;
        }
    }

    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
        restartLoop();
        return active_;
    }
    if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
        Log::error("the decoder reported an error mid-playback");
        return false;
    }
    if (!sample) {
        // A gap or a format change with no sample attached. Neither is fatal
        // and both resolve on the next read.
        return true;
    }

    // Track position so a teardown/resume cycle continues where it left off.
    if (timestamp > 0) resumeTimeHns_ = timestamp;

    // Held across the whole frame: the immediate context is shared with every
    // other monitor's render thread, and interleaving two monitors' binds and
    // draws would have each present the other's shader. See
    // D3DDevice::lockFrame.
    const auto frameLock = device_->lockFrame();

    if (!bindFrameTextures(sample.Get())) return true;
    if (!target_->beginFrame()) return false;

    D3DDevice::FrameConstants constants;
    const FitScale scale =
        fitScale(static_cast<FitMode>(fitMode_.load()), frameWidth_, frameHeight_,
                 target_->width(), target_->height());
    constants.fitScaleX = scale.x;
    constants.fitScaleY = scale.y;
    device_->bindPipeline(constants);

    ID3D11DeviceContext1* context = device_->context();
    ID3D11ShaderResourceView* views[2] = {lumaView_.Get(), chromaView_.Get()};
    context->PSSetShaderResources(0, 2, views);
    context->PSSetShader(bitDepth_ >= 10 ? device_->p010PS() : device_->nv12PS(), nullptr, 0);
    context->Draw(3, 0);

    // Unbind before presenting. The decoder recycles the texture this view
    // points at, and leaving it bound means the next decode writes into a
    // resource the pipeline still references — which the runtime resolves by
    // silently allocating another one.
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    context->PSSetShaderResources(0, 2, none);

    if (!target_->present(syncInterval_)) return false;

    ++framesPresented_;
    if (framesPresented_ % 240 == 0) {
        Log::info(format("frames=%lld t=%.1fs %s", framesPresented_,
                         static_cast<double>(timestamp) / kHnsPerSecond,
                         Footprint::formatted().c_str()));
    }
    return true;
}

bool VideoSource::bindFrameTextures(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return false;

    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (FAILED(buffer.As(&dxgiBuffer))) {
        // A system-memory buffer means DXVA did not engage — a software
        // decoder, or a driver that refused the device manager. Playing it
        // would mean a per-frame upload and a CPU decode, which is the exact
        // cost profile this app exists to avoid, so it stops instead and says
        // why.
        Log::error("the decoder returned a system-memory frame — hardware decode is not "
                   "available for this file on this machine");
        return false;
    }

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture)))) return false;

    UINT subresource = 0;
    dxgiBuffer->GetSubresourceIndex(&subresource);

    // The decoder's output is a texture *array*, and each decoded frame is one
    // slice of it.
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);

    const bool tenBit = (description.Format == DXGI_FORMAT_P010 ||
                         description.Format == DXGI_FORMAT_P016);
    bitDepth_ = tenBit ? 10 : 8;

    // Some drivers (seen on Intel UHD 630) allocate that array with
    // D3D11_BIND_DECODER only — no D3D11_BIND_SHADER_RESOURCE — so a view can
    // never be built on it directly, no matter what the reader's attributes
    // ask for at open time: CreateShaderResourceView fails with E_INVALIDARG
    // on every frame. The portable fix is the one every D3D11 video renderer
    // ends up at: copy the decoded slice, on the GPU, into a plain texture
    // this app allocates and binds itself. Recreated only when the frame size
    // or format changes, not per frame.
    D3D11_TEXTURE2D_DESC current{};
    if (shaderTexture_) shaderTexture_->GetDesc(&current);
    if (!shaderTexture_ || current.Width != description.Width ||
        current.Height != description.Height || current.Format != description.Format) {
        D3D11_TEXTURE2D_DESC copyDescription{};
        copyDescription.Width = description.Width;
        copyDescription.Height = description.Height;
        copyDescription.MipLevels = 1;
        copyDescription.ArraySize = 1;
        copyDescription.Format = description.Format;
        copyDescription.SampleDesc = {1, 0};
        copyDescription.Usage = D3D11_USAGE_DEFAULT;
        copyDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        shaderTexture_.Reset();
        const HRESULT hr =
            device_->device()->CreateTexture2D(&copyDescription, nullptr, &shaderTexture_);
        if (FAILED(hr)) {
            Log::error("could not allocate a shader-viewable copy of the decoded frame: " +
                      Log::hresult(hr));
            return false;
        }
    }

    device_->context()->CopySubresourceRegion(shaderTexture_.Get(), 0, 0, 0, 0, texture.Get(),
                                              subresource, nullptr);

    D3D11_SHADER_RESOURCE_VIEW_DESC luma{};
    luma.Format = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    luma.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    luma.Texture2D.MipLevels = 1;

    D3D11_SHADER_RESOURCE_VIEW_DESC chroma = luma;
    chroma.Format = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

    lumaView_.Reset();
    chromaView_.Reset();
    const HRESULT lumaHr = device_->device()->CreateShaderResourceView(shaderTexture_.Get(), &luma,
                                                                       &lumaView_);
    const HRESULT chromaHr = device_->device()->CreateShaderResourceView(
        shaderTexture_.Get(), &chroma, &chromaView_);
    if (FAILED(lumaHr) || FAILED(chromaHr)) {
        Log::error("could not view the decoded frame's planes: luma " + Log::hresult(lumaHr) +
                  ", chroma " + Log::hresult(chromaHr));
        return false;
    }
    return true;
}

void VideoSource::restartLoop() {
    resumeTimeHns_ = 0;
    if (!active_) return;

    // Seamless restart from the top. Recreating the reader rather than seeking
    // it is deliberate: SetCurrentPosition on a reader that has already reported
    // end-of-stream is documented to work and, on several drivers, returns
    // samples from before the seek for a frame or two.
    if (!createReader(0)) {
        Log::error("could not restart the loop");
        active_ = false;
    }
}

}  // namespace livewall
