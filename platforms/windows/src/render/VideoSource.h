// Plays a looping video into one monitor's swap chain.
//
// Memory discipline, in order of how much each one saves — the same three
// findings as the macOS version, expressed in Media Foundation instead of
// AVFoundation:
//
//  1. **One frame in flight.** A pacing thread pulls exactly one sample per
//     tick, draws it and presents it. There is no read-ahead queue and no
//     media clock. `IMFSourceReader` in synchronous mode does exactly this;
//     the asynchronous callback mode is the `AVPlayer` of this API — it reads
//     ahead by an amount you do not control, which at 4K 10-bit is ~20 MB a
//     frame.
//
//  2. **The decoder's own format, converted in a shader.** The decoder produces
//     NV12 at 8-bit and P010 at 10-bit, which is 1.5 and 3 bytes per pixel
//     against BGRA's 4, and it produces them without being asked. Setting
//     MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING would insert the video
//     processor MFT to hand back RGB, allocating a second full-size pool and —
//     on machines where it does not hit the fixed-function path — costing more
//     CPU than the decode. This is the Windows shape of the macOS finding that
//     naming a pixel format made AVFoundation revalidate every buffer through a
//     kernel round trip. YUV to RGB is four multiply-adds per pixel on hardware
//     that does nothing else.
//
//  3. **Teardown on deactivate.** Losing visibility destroys the source reader
//     and its decoder outright. The last presented frame stays in the swap
//     chain's front buffer at no cost, and playback resumes from the saved
//     timestamp.
//
// Assets are expected to have been normalised by `Transcoder` first: HEVC or
// H.264, no audio track, capped resolution and frame rate, no B-frames.
#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "render/WallpaperSource.h"

namespace livewall {

class VideoSource final : public WallpaperSource {
public:
    VideoSource(std::shared_ptr<D3DDevice> device, std::wstring path, int fps, int bitDepth,
                FitMode fitMode);
    ~VideoSource() override;

    // Opens the file and reads its real frame rate and dimensions. Returns
    // false when the file is missing, has no video track, or names a codec no
    // decoder on this machine handles.
    //
    // Separate from the constructor because it touches the disk, and the caller
    // wants to fall back to the procedural mode rather than install a source
    // that cannot play.
    bool prepare();

    void attach(SwapChainTarget* target, int refreshHz) override;
    void activate() override;
    void deactivate() override;
    bool isActive() const override { return active_; }
    void setFitMode(FitMode mode) override;
    std::string summary() const override;

    int frameWidth() const { return frameWidth_; }
    int frameHeight() const { return frameHeight_; }
    int framesPerSecond() const { return fps_; }

private:
    bool createReader(long long startTimeHns);
    void releaseReader();
    void renderLoop();
    bool decodeAndDrawOneFrame();
    bool bindFrameTextures(IMFSample* sample);
    void restartLoop();

    std::shared_ptr<D3DDevice> device_;
    std::wstring path_;

    // Seeded from the library's record but replaced by the track's real rate
    // once the file is opened: pulling one frame per tick means a mismatch
    // between the two plays the clip at the wrong speed, and the file is the
    // authority, not the index.
    int fps_ = 24;

    // Recorded by the importer rather than sniffed from the file: we encoded
    // it, so we know, and the format description is only consulted to confirm.
    int bitDepth_ = 8;

    int frameWidth_ = 0;
    int frameHeight_ = 0;
    long long durationHns_ = 0;

    SwapChainTarget* target_ = nullptr;
    int refreshHz_ = 60;

    // How many vertical blanks Present waits for. Non-zero only when the frame
    // rate divides the refresh rate exactly, in which case DXGI does the pacing
    // and the thread sleeps in the driver rather than in a timer.
    UINT syncInterval_ = 0;

    ComPtr<IMFSourceReader> reader_;
    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    UINT deviceManagerToken_ = 0;

    // Recreated per frame, because the decoder hands back a different slice of
    // its texture array each time.
    ComPtr<ID3D11ShaderResourceView> lumaView_;
    ComPtr<ID3D11ShaderResourceView> chromaView_;

    std::thread thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stopping_{false};
    HANDLE timer_ = nullptr;
    // Manual-reset, signalled to break the frame wait immediately.
    //
    // CancelWaitableTimer does *not* wake a thread already blocked on the
    // timer — it only stops the timer from ever signalling, which turns a
    // pending wait into a permanent one and would deadlock the join in
    // `deactivate`. Waiting on both handles is what makes teardown return.
    HANDLE stopEvent_ = nullptr;

    // Guards `reader_` between the render thread and a deactivate arriving from
    // the UI thread.
    std::mutex readerMutex_;

    // Playback position preserved across teardown so resuming continues the
    // loop instead of snapping back to frame zero.
    std::atomic<long long> resumeTimeHns_{0};

    std::atomic<int> fitMode_{static_cast<int>(kDefaultFitMode)};
    long long framesPresented_ = 0;
};

}  // namespace livewall
