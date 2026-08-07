#include "render/GradientSource.h"

#include "support/Footprint.h"
#include "support/Log.h"

namespace livewall {

GradientSource::GradientSource(std::shared_ptr<D3DDevice> device)
    : device_(std::move(device)) {
    stopEvent_ = CreateEventW(nullptr, /*manualReset=*/TRUE, /*initial=*/FALSE, nullptr);

    // A high-resolution timer, so 15 fps means 15 fps rather than whatever the
    // 15.6 ms system tick rounds it to. The alternative — raising the global
    // timer resolution with timeBeginPeriod — would speed up every other
    // process's timers too and costs the whole machine battery life.
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (timer_ == nullptr) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
}

GradientSource::~GradientSource() {
    deactivate();
    if (timer_ != nullptr) CloseHandle(timer_);
    if (stopEvent_ != nullptr) CloseHandle(stopEvent_);
}

void GradientSource::attach(SwapChainTarget* target, int refreshHz) {
    const bool wasActive = active_;
    if (wasActive) deactivate();

    target_ = target;
    refreshHz_ = refreshHz > 0 ? refreshHz : 60;

    if (wasActive) activate();
}

void GradientSource::activate() {
    if (active_ || target_ == nullptr || !device_) return;

    stopping_ = false;
    if (stopEvent_ != nullptr) ResetEvent(stopEvent_);
    active_ = true;
    thread_ = std::thread([this] { renderLoop(); });
    Log::info("gradient activated — " + Footprint::formatted());
}

void GradientSource::deactivate() {
    if (!active_) return;

    stopping_ = true;
    active_ = false;
    // Break the frame wait immediately rather than letting the thread sit out
    // the rest of its 66 ms, which is what makes deactivation feel instant when
    // a window is dragged over the desktop.
    if (stopEvent_ != nullptr) SetEvent(stopEvent_);
    if (timer_ != nullptr) CancelWaitableTimer(timer_);
    if (thread_.joinable()) thread_.join();

    // Deliberately no final clear: the last frame stays in the swap chain's
    // front buffer, so a stopped gradient looks like a still rather than a
    // hole where the wallpaper was.
    Log::info("gradient deactivated — " + Footprint::formatted());
}

void GradientSource::renderLoop() {
    const auto started = std::chrono::steady_clock::now();
    const double startOffset = elapsedSeconds_;

    LARGE_INTEGER period{};
    // Negative means relative; units are 100 ns.
    period.QuadPart = -(10'000'000LL / kFramesPerSecond);

    while (!stopping_) {
        if (timer_ != nullptr && stopEvent_ != nullptr) {
            SetWaitableTimer(timer_, &period, 0, nullptr, nullptr, FALSE);
            const HANDLE waits[2] = {stopEvent_, timer_};
            const DWORD signalled =
                WaitForMultipleObjects(2, waits, /*waitAll=*/FALSE, INFINITE);
            if (signalled == WAIT_OBJECT_0) break;  // stopEvent_
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / kFramesPerSecond));
        }
        if (stopping_) break;

        const double now =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        elapsedSeconds_ = startOffset + now;
        drawOnce(elapsedSeconds_);
    }
}

void GradientSource::drawOnce(double seconds) {
    if (target_ == nullptr) return;

    target_->waitForPresentable();
    if (stopping_) return;

    // Held across the whole frame: the immediate context is shared with every
    // other monitor's render thread. See D3DDevice::lockFrame.
    const auto frameLock = device_->lockFrame();
    if (!target_->beginFrame()) return;

    D3DDevice::FrameConstants constants;
    // The gradient fills whatever it is given, so no fit transform applies.
    constants.fitScaleX = 1.0f;
    constants.fitScaleY = 1.0f;
    constants.time = static_cast<float>(seconds);
    device_->bindPipeline(constants);

    ID3D11DeviceContext1* context = device_->context();
    context->PSSetShader(device_->gradientPS(), nullptr, 0);
    context->Draw(3, 0);

    // Sync interval 0: the loop paces itself from the timer, and asking DXGI to
    // wait for a vertical blank on top of that would quantise 15 fps up to the
    // refresh rate's nearest divisor and drift against the timer.
    target_->present(0);
}

}  // namespace livewall
