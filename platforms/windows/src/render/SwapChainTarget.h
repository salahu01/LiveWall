// One monitor's swap chain, bound into that monitor's DirectComposition visual.
//
// Composition rather than an HWND swap chain, for one reason that matters: an
// HWND swap chain is opaque. In Fit mode the letterbox bars have to show the
// user's real desktop wallpaper — which is what the macOS version gets for free
// by never painting an opaque layer — and only a composition swap chain with
// premultiplied alpha can leave a region genuinely transparent.
#pragma once

#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <memory>

#include "render/D3DDevice.h"
#include "render/DesktopHost.h"

namespace livewall {

class SwapChainTarget {
public:
    static std::unique_ptr<SwapChainTarget> create(std::shared_ptr<D3DDevice> device,
                                                   DesktopHost& host);

    ~SwapChainTarget();

    SwapChainTarget(const SwapChainTarget&) = delete;
    SwapChainTarget& operator=(const SwapChainTarget&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }

    // Binds the back buffer as the render target and clears it to fully
    // transparent. The clear is what makes Fit mode's bars show the system
    // wallpaper: anything not drawn afterwards stays transparent.
    bool beginFrame();

    // Presents. `syncInterval` is how many vertical blanks to wait: the caller
    // passes refresh/fps when the frame rate divides the refresh rate exactly,
    // which lets DXGI do the pacing and the app's thread sleep in the driver
    // rather than in a timer. Otherwise 0, and the caller paces itself.
    //
    // Returns false when the device was lost, which is the caller's cue to tear
    // everything down and rebuild.
    bool present(UINT syncInterval);

    // Resizes the buffers after a resolution change. Cheaper than recreating
    // the swap chain and, more to the point, does not disturb the composition
    // visual it is bound to.
    bool resize(int width, int height);

    // Blocks until the swap chain can accept another frame. With a maximum
    // frame latency of 1 this returns as soon as the previously presented frame
    // has been consumed, which is the mechanism that holds the app to one frame
    // in flight without a queue of its own.
    void waitForPresentable();

private:
    SwapChainTarget() = default;
    bool initialise(std::shared_ptr<D3DDevice> device, DesktopHost& host);
    bool createRenderTarget();

    std::shared_ptr<D3DDevice> device_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    HANDLE frameLatencyWaitable_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace livewall
