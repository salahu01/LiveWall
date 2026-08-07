// Plays one converted wallpaper file into one output's surface.
//
// The three properties this shares with the macOS port, which are what make the
// numbers work:
//
//   One frame in flight. `render()` pulls exactly one frame per tick and draws
//   it. There is no read-ahead queue and no synchroniser with a timebase, so
//   the decoder's surface pool sits at its floor. The macOS port measured the
//   alternative — a renderer that reads ahead by an amount you cannot control —
//   at 28 MB against 19 MB for no CPU improvement, and one 10-bit 4K frame is
//   ~20 MB, so the same arithmetic applies here.
//
//   Teardown, not pause. `deactivate()` closes the codec context, releases the
//   VA-API surfaces and drops the demuxer, keeping only the timestamp.
//   `activate()` reopens and seeks back. A covered wallpaper costs nothing.
//
//   Zero-copy where the hardware allows it. VA-API decodes into a surface that
//   is exported as a dmabuf, imported as an EGLImage and sampled as an external
//   texture. The frame never enters system memory and the YUV-to-RGB conversion
//   happens on the sampler.
//
// The fallback path is the honest asterisk on that last one. Without VA-API, or
// without EGL_EXT_image_dma_buf_import, frames are decoded in software and
// converted with swscale before upload — which costs roughly an order of
// magnitude more CPU. `summary()` says which path is live and `livewall status`
// prints it, because the difference is large enough that a user should not have
// to guess.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "import/FFmpeg.h"
#include "render/EglDevice.h"
#include "render/GlProgram.h"
#include "render/WallpaperSource.h"

namespace livewall {

class VideoSource final : public WallpaperSource {
public:
    VideoSource(std::string path, int fps, int bitDepth);
    ~VideoSource() override;

    bool prepare(EglDevice& egl) override;
    void activate() override;
    void deactivate() override;
    bool isActive() const override { return active_; }

    int framesPerSecond() const override { return fps_; }
    bool render(Surface& surface, FitMode mode) override;

    int contentWidth() const override { return width_; }
    int contentHeight() const override { return height_; }
    std::string summary() const override;

private:
    // How a decoded frame reaches the GPU. Decided once, when the decoder
    // opens, and reported in `summary()`.
    // `Idle` rather than `None` for the same reason as dbus::Value::Kind: None
    // is an Xlib macro.
    enum class Path { Idle, DmaBuf, Upload };

    bool openDecoder();
    void closeDecoder();
    bool openHardwareDevice();

    // Pulls exactly one frame, looping the file at EOF. False means no frame
    // was available this tick, which is not an error.
    bool decodeOneFrame();

    bool uploadFrame(AVFrame* frame);
    bool mapFrameAsImage(AVFrame* frame);
    void releaseImage();

    void ensureTexture(GLenum target);

    std::string filePath_;
    int fps_ = 24;
    int bitDepth_ = 8;
    int width_ = 0;
    int height_ = 0;

    EglDevice* egl_ = nullptr;
    GlProgram externalProgram_;
    GlProgram uploadProgram_;

    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* softwareFrame_ = nullptr;
    AVFrame* drmFrame_ = nullptr;
    SwsContext* scaler_ = nullptr;
    std::unique_ptr<std::uint8_t[]> rgbaBuffer_;
    int streamIndex_ = -1;

    EGLImageKHR image_ = EGL_NO_IMAGE_KHR;
    GLuint texture_ = 0;
    GLenum textureTarget_ = 0;

    Path pathKind_ = Path::Idle;
    bool active_ = false;
    bool prepared_ = false;

    // Where playback resumes after a teardown. In the stream's own time base,
    // which is why it is stored rather than converted — a seek wants it in the
    // same units it came out in.
    std::int64_t resumeTimestamp_ = 0;
};

}  // namespace livewall
