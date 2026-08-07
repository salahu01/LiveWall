#include "render/SwapChainTarget.h"

#include "support/Log.h"
#include "support/Strings.h"

namespace livewall {

std::unique_ptr<SwapChainTarget> SwapChainTarget::create(std::shared_ptr<D3DDevice> device,
                                                         DesktopHost& host) {
    std::unique_ptr<SwapChainTarget> target(new SwapChainTarget());
    if (!target->initialise(std::move(device), host)) return nullptr;
    return target;
}

SwapChainTarget::~SwapChainTarget() {
    if (frameLatencyWaitable_ != nullptr) CloseHandle(frameLatencyWaitable_);
}

bool SwapChainTarget::initialise(std::shared_ptr<D3DDevice> device, DesktopHost& host) {
    device_ = std::move(device);
    if (!device_ || device_->factory() == nullptr) return false;

    width_ = host.width();
    height_ = host.height();
    if (width_ <= 0 || height_ <= 0) return false;

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = static_cast<UINT>(width_);
    description.Height = static_cast<UINT>(height_);
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Two buffers, the minimum a flip model allows. Three would let the app run
    // a frame further ahead of the display, which is precisely what this design
    // does not want.
    description.BufferCount = 2;
    // FLIP_SEQUENTIAL rather than FLIP_DISCARD: with DISCARD the contents of a
    // back buffer are undefined after a present, and this app deliberately
    // stops presenting when the desktop is covered. The last frame has to keep
    // standing, and SEQUENTIAL is what guarantees the buffer it lives in is not
    // recycled underneath it.
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    description.Scaling = DXGI_SCALING_STRETCH;
    // The waitable object is how the app blocks until the previous frame has
    // actually been consumed, rather than guessing with a timer.
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    HRESULT hr = device_->factory()->CreateSwapChainForComposition(
        device_->device(), &description, nullptr, &swapChain_);
    if (FAILED(hr)) {
        Log::error("CreateSwapChainForComposition failed: " + Log::hresult(hr));
        return false;
    }

    ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(swapChain_.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(1);
        frameLatencyWaitable_ = swapChain2->GetFrameLatencyWaitableObject();
    }

    if (host.visual() == nullptr) return false;
    hr = host.visual()->SetContent(swapChain_.Get());
    if (FAILED(hr)) {
        Log::error("could not attach the swap chain to the composition visual: " +
                   Log::hresult(hr));
        return false;
    }
    host.commit();

    return createRenderTarget();
}

bool SwapChainTarget::createRenderTarget() {
    renderTarget_.Reset();

    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        Log::error("could not get the swap chain back buffer: " + Log::hresult(hr));
        return false;
    }

    hr = device_->device()->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_);
    if (FAILED(hr)) {
        Log::error("could not create a render target view: " + Log::hresult(hr));
        return false;
    }
    return true;
}

bool SwapChainTarget::beginFrame() {
    if (!renderTarget_) return false;

    ID3D11DeviceContext1* context = device_->context();

    // Fully transparent, premultiplied. Everything the shaders do not cover
    // stays this way, which is what puts the user's own wallpaper in the
    // letterbox bars instead of black.
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context->ClearRenderTargetView(renderTarget_.Get(), clear);
    context->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    return true;
}

bool SwapChainTarget::present(UINT syncInterval) {
    if (!swapChain_) return false;

    const HRESULT hr = swapChain_->Present(syncInterval, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        Log::error("the graphics device was lost during present: " + Log::hresult(hr));
        return false;
    }
    if (FAILED(hr)) {
        Log::error("present failed: " + Log::hresult(hr));
        return false;
    }
    return true;
}

bool SwapChainTarget::resize(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (width == width_ && height == height_) return true;

    // Every view onto the buffers has to be released before ResizeBuffers, or
    // it fails with DXGI_ERROR_INVALID_CALL and the swap chain is left in a
    // state nothing recovers from.
    renderTarget_.Reset();
    device_->context()->ClearState();
    device_->context()->Flush();

    const HRESULT hr = swapChain_->ResizeBuffers(
        0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) {
        Log::error("ResizeBuffers failed: " + Log::hresult(hr));
        return false;
    }

    width_ = width;
    height_ = height;
    return createRenderTarget();
}

void SwapChainTarget::waitForPresentable() {
    if (frameLatencyWaitable_ == nullptr) return;
    // 1000 ms rather than INFINITE: a driver that stops signalling would
    // otherwise hang the decode thread forever, and the caller's next loop
    // iteration re-checks whether it should still be running at all.
    WaitForSingleObjectEx(frameLatencyWaitable_, 1000, TRUE);
}

}  // namespace livewall
