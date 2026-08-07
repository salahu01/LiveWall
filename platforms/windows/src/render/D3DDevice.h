// The one Direct3D 11 device the whole app shares, plus the shaders and state
// objects that hang off it.
//
// Shared rather than per-monitor because the device is the expensive object —
// creating it costs a driver round trip and a few megabytes — while a swap
// chain is cheap. Two monitors therefore mean two swap chains and two decoders
// (see WallpaperEngine for why the decoders are not shared) over one device.
//
// The device is created lazily and torn down when the last user goes away, so a
// procedural-only session on a machine with no video selected never touches the
// GPU driver at all.
#pragma once

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>

namespace livewall {

using Microsoft::WRL::ComPtr;

class D3DDevice {
public:
    // Returns the shared device, creating it on first use. Null if D3D11 is
    // unavailable, which on a machine with no usable adapter it can be.
    static std::shared_ptr<D3DDevice> acquire();

    ~D3DDevice();

    D3DDevice(const D3DDevice&) = delete;
    D3DDevice& operator=(const D3DDevice&) = delete;

    ID3D11Device1* device() const { return device_.Get(); }
    ID3D11DeviceContext1* context() const { return context_.Get(); }
    IDXGIFactory2* factory() const { return factory_.Get(); }

    ID3D11VertexShader* fullscreenVS() const { return fullscreenVS_.Get(); }
    ID3D11PixelShader* nv12PS() const { return nv12PS_.Get(); }
    ID3D11PixelShader* p010PS() const { return p010PS_.Get(); }
    ID3D11PixelShader* gradientPS() const { return gradientPS_.Get(); }
    ID3D11SamplerState* linearClamp() const { return sampler_.Get(); }
    ID3D11Buffer* constants() const { return constants_.Get(); }

    // What the shaders' cbuffer b0 holds. 16-byte aligned by construction —
    // D3D rejects a constant buffer whose size is not a multiple of 16.
    struct FrameConstants {
        float fitScaleX = 1.0f;
        float fitScaleY = 1.0f;
        float time = 0.0f;
        float fullRange = 0.0f;
    };

    // Uploads `values` and binds the shared pipeline state. Called once per
    // frame per monitor, which at 24 fps on two displays is 48 map/unmap pairs
    // a second — small enough not to warrant a per-monitor buffer.
    void bindPipeline(const FrameConstants& values);

    // Held for the whole of one monitor's clear → bind → draw → present.
    //
    // Every monitor's render thread shares this device's immediate context, and
    // a context is a single piece of state: two threads interleaving their
    // binds and draws would each present the other's shader and constants.
    // `SetMultithreadProtected` makes individual calls safe, not sequences of
    // them, so the sequence is what gets the lock. A draw is a handful of
    // microseconds, so serialising two displays costs nothing measurable.
    [[nodiscard]] std::unique_lock<std::mutex> lockFrame() {
        return std::unique_lock<std::mutex>(frameMutex_);
    }

    // True when the device was removed — a driver reset, a GPU hot-swap, or
    // waking from hibernation with a different adapter. Every caller responds
    // by dropping its swap chain and asking for a new device.
    bool isLost() const;

private:
    D3DDevice() = default;
    bool initialise();

    ComPtr<ID3D11Device1> device_;
    ComPtr<ID3D11DeviceContext1> context_;
    ComPtr<IDXGIFactory2> factory_;

    ComPtr<ID3D11VertexShader> fullscreenVS_;
    ComPtr<ID3D11PixelShader> nv12PS_;
    ComPtr<ID3D11PixelShader> p010PS_;
    ComPtr<ID3D11PixelShader> gradientPS_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> rasteriser_;

    std::mutex frameMutex_;
};

}  // namespace livewall
