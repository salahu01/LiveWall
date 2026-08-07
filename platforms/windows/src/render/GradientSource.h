// The cheapest wallpaper the app can draw: three crossed sinusoids in a pixel
// shader, at 15 fps.
//
// This is the one place the Windows port is measurably more expensive than the
// macOS one, and the reason is worth writing down rather than discovering.
//
// On macOS the procedural mode is a `CAGradientLayer` with keyframes handed to
// the render server once. The compositor interpolates them on the GPU from then
// on, so the app's own cost is literally zero — no display link, no per-frame
// callback, no Metal device. Windows has no equivalent. DirectComposition can
// animate a transform, an opacity or a clip, but it cannot animate the colour
// stops of a gradient, and there is no compositor-side shader. So the choice is
// between a static picture and a small real cost, and this takes the small real
// cost: one full-screen triangle of ALU work, fifteen times a second, which
// measures under 1% of a core.
//
// 15 fps rather than the refresh rate because nothing in the field moves fast
// enough to tell the difference, and the cost is linear in frames.
#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include "render/WallpaperSource.h"

namespace livewall {

class GradientSource final : public WallpaperSource {
public:
    explicit GradientSource(std::shared_ptr<D3DDevice> device);
    ~GradientSource() override;

    void attach(SwapChainTarget* target, int refreshHz) override;
    void activate() override;
    void deactivate() override;
    bool isActive() const override { return active_; }
    std::string summary() const override { return "Procedural gradient"; }

private:
    void renderLoop();
    void drawOnce(double seconds);

    static constexpr int kFramesPerSecond = 15;

    std::shared_ptr<D3DDevice> device_;
    SwapChainTarget* target_ = nullptr;
    int refreshHz_ = 60;

    std::thread thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stopping_{false};
    HANDLE timer_ = nullptr;
    // Manual-reset, signalled to break the frame wait immediately.
    //
    // CancelWaitableTimer does *not* wake a thread already blocked on the
    // timer — it only stops the timer from ever signalling, which turns a
    // pending wait into a permanent one. Waiting on both handles is what makes
    // deactivation actually return.
    HANDLE stopEvent_ = nullptr;

    // Elapsed animation time, preserved across deactivate/activate so resuming
    // continues the drift rather than snapping back to the start.
    double elapsedSeconds_ = 0;
};

}  // namespace livewall
