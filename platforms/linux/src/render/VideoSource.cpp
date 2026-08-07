#include "render/VideoSource.h"

#include <unistd.h>
#include <algorithm>

#include "shaders/external_frag.h"
#include "shaders/rgb_frag.h"
#include "shaders/wallpaper_vert.h"

#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

// Chosen by the decoder's get_format callback. A file-scope value rather than a
// member because the callback has C linkage and only gets the AVCodecContext.
enum AVPixelFormat selectHardwareFormat(AVCodecContext* context,
                                        const enum AVPixelFormat* formats) {
    (void)context;
    for (const enum AVPixelFormat* candidate = formats; *candidate != AV_PIX_FMT_NONE; ++candidate) {
        if (*candidate == AV_PIX_FMT_VAAPI) return *candidate;
    }
    // Returning the first offered format is how FFmpeg is told "no hardware
    // then, software is fine". Returning AV_PIX_FMT_NONE would fail the decode
    // instead, which is the wrong answer on a machine with no VA-API.
    return formats[0];
}

// The render nodes, in the order worth trying. renderD128 is the first GPU on
// every normal machine; the loop covers a second GPU and the hybrid-laptop case
// where the integrated one is not first.
constexpr const char* kRenderNodes[] = {
    "/dev/dri/renderD128",
    "/dev/dri/renderD129",
    "/dev/dri/renderD130",
};

}  // namespace

VideoSource::VideoSource(std::string path, int fps, int bitDepth)
    : filePath_(std::move(path)), fps_(fps > 0 ? fps : 24), bitDepth_(bitDepth) {}

VideoSource::~VideoSource() {
    closeDecoder();
    releaseImage();
    if (texture_ != 0) glDeleteTextures(1, &texture_);
}

bool VideoSource::prepare(EglDevice& egl) {
    egl_ = &egl;

    if (!ffmpeg::load()) {
        Log::error("no FFmpeg, so " + paths::filename(filePath_) + " cannot be played");
        return false;
    }
    if (!paths::fileExists(filePath_)) {
        Log::error(filePath_ + " is gone");
        return false;
    }

    // Opened, measured and closed again. The dimensions are needed now — the
    // fit-mode arithmetic and the status line both want them — but holding a
    // demuxer open for a wallpaper that may never become visible would defeat
    // the point of tearing down on occlusion.
    const ffmpeg::Api& av = ffmpeg::api();
    AVFormatContext* probe = nullptr;
    int result = av.open_input(&probe, filePath_.c_str(), nullptr, nullptr);
    if (result < 0) {
        Log::error("cannot open " + paths::filename(filePath_) + ": " + ffmpeg::errorText(result));
        return false;
    }

    if (av.find_stream_info(probe, nullptr) < 0) {
        av.close_input(&probe);
        Log::error("no stream information in " + paths::filename(filePath_));
        return false;
    }

    const int stream = av.find_best_stream(probe, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) {
        av.close_input(&probe);
        Log::error(paths::filename(filePath_) + " has no video track");
        return false;
    }

    width_ = probe->streams[stream]->codecpar->width;
    height_ = probe->streams[stream]->codecpar->height;
    av.close_input(&probe);

    if (width_ <= 0 || height_ <= 0) return false;

    // Both programs are compiled up front. Compiling on first use would put a
    // shader compile inside the first frame after every occlusion resume, which
    // is exactly the moment the user is looking.
    if (!externalProgram_.build("video-external", kwallpaper_vert, kexternal_frag)) {
        // A driver with no GL_OES_EGL_image_external. The upload path does not
        // need it.
        Log::info("no external-image sampler — the dmabuf path is unavailable");
    }
    if (!uploadProgram_.build("video-upload", kwallpaper_vert, krgb_frag)) return false;

    prepared_ = true;
    return true;
}

bool VideoSource::openHardwareDevice() {
    const ffmpeg::Api& av = ffmpeg::api();

    for (const char* node : kRenderNodes) {
        if (::access(node, R_OK | W_OK) != 0) continue;
        const int result =
            av.hwdevice_ctx_create(&hwDevice_, AV_HWDEVICE_TYPE_VAAPI, node, nullptr, 0);
        if (result >= 0) {
            Log::info(std::string("VA-API on ") + node);
            return true;
        }
        Log::info(std::string("VA-API unavailable on ") + node + ": " + ffmpeg::errorText(result));
    }

    // Not an error. A machine with no render node — a VM, a container, an
    // NVIDIA card with the proprietary driver and no VA-API bridge — decodes in
    // software, which works and costs more.
    Log::info("no VA-API device — decoding in software");
    return false;
}

bool VideoSource::openDecoder() {
    const ffmpeg::Api& av = ffmpeg::api();

    int result = av.open_input(&format_, filePath_.c_str(), nullptr, nullptr);
    if (result < 0) {
        Log::error("cannot open " + paths::filename(filePath_) + ": " + ffmpeg::errorText(result));
        return false;
    }
    if (av.find_stream_info(format_, nullptr) < 0) return false;

    const AVCodec* decoder = nullptr;
    streamIndex_ = av.find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (streamIndex_ < 0 || decoder == nullptr) return false;

    codec_ = av.alloc_context(decoder);
    if (codec_ == nullptr) return false;

    if (av.parameters_to_context(codec_, format_->streams[streamIndex_]->codecpar) < 0) return false;

    // One thread. The obvious setting is "as many as there are cores", and it
    // is wrong here for the same reason the read-ahead queue is: frame-threaded
    // decoding keeps several frames in flight to have something for each
    // thread, and this pump wants exactly one. At 24 fps of already-downscaled
    // HEVC there is nothing to parallelise anyway.
    codec_->thread_count = 1;

    if (openHardwareDevice()) {
        codec_->hw_device_ctx = av.buffer_ref(hwDevice_);
        codec_->get_format = selectHardwareFormat;
    }

    result = av.open2(codec_, decoder, nullptr);
    if (result < 0) {
        Log::error("cannot open the decoder: " + ffmpeg::errorText(result));
        return false;
    }

    packet_ = av.packet_alloc();
    frame_ = av.frame_alloc();
    softwareFrame_ = av.frame_alloc();
    drmFrame_ = av.frame_alloc();
    if (packet_ == nullptr || frame_ == nullptr || softwareFrame_ == nullptr ||
        drmFrame_ == nullptr) {
        return false;
    }

    // The decision, made once. Which path is live changes the CPU figure by
    // about an order of magnitude, so it is reported rather than inferred.
    const bool canMap = hwDevice_ != nullptr && egl_ != nullptr &&
                        egl_->supportsDmaBufImport() && externalProgram_.valid();
    pathKind_ = canMap ? Path::DmaBuf : Path::Upload;

    if (resumeTimestamp_ > 0) {
        // AVSEEK_FLAG_BACKWARD lands on the keyframe at or before the target.
        // The wallpaper's keyframes are five seconds apart by design, so a
        // resume can replay up to five seconds — which nobody notices on
        // ambient content and which is the trade the sparse-keyframe choice
        // already made at import time.
        av.seek_frame(format_, streamIndex_, resumeTimestamp_, AVSEEK_FLAG_BACKWARD);
        av.flush_buffers(codec_);
    }

    return true;
}

void VideoSource::closeDecoder() {
    if (!ffmpeg::load()) return;
    const ffmpeg::Api& av = ffmpeg::api();

    releaseImage();

    if (packet_ != nullptr) av.packet_free(&packet_);
    if (frame_ != nullptr) av.frame_free(&frame_);
    if (softwareFrame_ != nullptr) av.frame_free(&softwareFrame_);
    if (drmFrame_ != nullptr) av.frame_free(&drmFrame_);
    if (scaler_ != nullptr) {
        av.sws_free_context(scaler_);
        scaler_ = nullptr;
    }
    rgbaBuffer_.reset();

    // Order matters: the codec context holds a reference to the hardware
    // device, and freeing the device first leaves it with a dangling one that
    // it dereferences while closing.
    if (codec_ != nullptr) av.free_context(&codec_);
    if (hwDevice_ != nullptr) av.buffer_unref(&hwDevice_);
    if (format_ != nullptr) av.close_input(&format_);

    streamIndex_ = -1;
    pathKind_ = Path::Idle;
}

void VideoSource::activate() {
    if (active_ || !prepared_) return;
    if (!openDecoder()) {
        closeDecoder();
        Log::error("could not start playback of " + paths::filename(filePath_));
        return;
    }
    active_ = true;
}

void VideoSource::deactivate() {
    if (!active_) return;
    active_ = false;
    // Everything goes. This is the whole cost model: the surfaces, the codec's
    // internal pool and the demuxer's buffers are all released, and what stays
    // resident is a timestamp.
    closeDecoder();
}

bool VideoSource::decodeOneFrame() {
    const ffmpeg::Api& av = ffmpeg::api();

    for (int attempt = 0; attempt < 256; ++attempt) {
        const int received = av.receive_frame(codec_, frame_);
        if (received == 0) {
            if (frame_->best_effort_timestamp != AV_NOPTS_VALUE) {
                resumeTimestamp_ = frame_->best_effort_timestamp;
            }
            return true;
        }
        if (received != AVERROR(EAGAIN) && received != AVERROR_EOF) {
            Log::error("decode failed: " + ffmpeg::errorText(received));
            return false;
        }

        const int read = av.read_frame(format_, packet_);
        if (read == AVERROR_EOF) {
            // Loop. The whole library is short ambient clips, so this fires
            // regularly and has to be cheap: a seek to zero and a codec flush,
            // not a reopen.
            av.seek_frame(format_, streamIndex_, 0, AVSEEK_FLAG_BACKWARD);
            av.flush_buffers(codec_);
            resumeTimestamp_ = 0;
            continue;
        }
        if (read < 0) {
            Log::error("read failed: " + ffmpeg::errorText(read));
            return false;
        }

        if (packet_->stream_index == streamIndex_) {
            av.send_packet(codec_, packet_);
        }
        av.packet_unref(packet_);
    }

    // 256 packets without a frame means something is wrong with the file rather
    // than with this tick. Bounded so a corrupt file cannot spin the event loop
    // forever.
    Log::error("no frame after 256 packets — giving up on this tick");
    return false;
}

void VideoSource::ensureTexture(GLenum target) {
    if (texture_ != 0 && textureTarget_ == target) return;
    if (texture_ != 0) glDeleteTextures(1, &texture_);

    glGenTextures(1, &texture_);
    textureTarget_ = target;
    glBindTexture(target, texture_);
    // No mipmaps and clamped edges. A wallpaper is drawn at close to 1:1, and
    // GL_LINEAR without mipmaps is both what looks right and what an external
    // image supports — GL_TEXTURE_EXTERNAL_OES has no mip levels at all.
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void VideoSource::releaseImage() {
    if (image_ != EGL_NO_IMAGE_KHR && egl_ != nullptr) {
        egl_->destroyImage(image_);
        image_ = EGL_NO_IMAGE_KHR;
    }
    // The mapped frame owns the dmabuf descriptors the image was built from, so
    // it is unreferenced only after the image is gone. The other order closes
    // the file descriptors while EGL still holds them.
    if (drmFrame_ != nullptr && ffmpeg::load()) ffmpeg::api().frame_unref(drmFrame_);
}

bool VideoSource::mapFrameAsImage(AVFrame* frame) {
    const ffmpeg::Api& av = ffmpeg::api();

    releaseImage();

    drmFrame_->format = AV_PIX_FMT_DRM_PRIME;
    // AV_HWFRAME_MAP_DIRECT asks for the decoder's own surface rather than a
    // copy of it. Without it some drivers satisfy the map by allocating a
    // second surface and blitting, which is the copy this whole path exists to
    // avoid.
    const int mapped = av.hwframe_map(drmFrame_, frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
    if (mapped < 0) {
        Log::info("dmabuf map failed (" + ffmpeg::errorText(mapped) + ") — falling back to upload");
        pathKind_ = Path::Upload;
        return false;
    }

    const auto* descriptor = reinterpret_cast<const AVDRMFrameDescriptor*>(drmFrame_->data[0]);
    if (descriptor == nullptr || descriptor->nb_layers != 1) {
        // A multi-layer descriptor is NV12 split into a separate R8 and GR88
        // image, which needs two textures and a colour matrix in the shader.
        // Supporting it would mean choosing a matrix by hand for every colour
        // space; the upload path is correct without that choice, so this falls
        // back rather than guessing.
        Log::info("multi-layer dmabuf descriptor — falling back to upload");
        pathKind_ = Path::Upload;
        return false;
    }

    const AVDRMLayerDescriptor& layer = descriptor->layers[0];

    EGLint attributes[64];
    int index = 0;
    attributes[index++] = EGL_WIDTH;
    attributes[index++] = frame->width;
    attributes[index++] = EGL_HEIGHT;
    attributes[index++] = frame->height;
    attributes[index++] = EGL_LINUX_DRM_FOURCC_EXT;
    attributes[index++] = static_cast<EGLint>(layer.format);

    // The per-plane attribute names are not contiguous enum values, so they are
    // spelled out rather than computed from the plane index.
    static constexpr EGLint kFdAttribute[4] = {
        EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT};
    static constexpr EGLint kOffsetAttribute[4] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static constexpr EGLint kPitchAttribute[4] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static constexpr EGLint kModifierLoAttribute[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static constexpr EGLint kModifierHiAttribute[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

    const int planes = std::min(layer.nb_planes, 4);
    for (int plane = 0; plane < planes; ++plane) {
        const AVDRMPlaneDescriptor& description = layer.planes[plane];
        const AVDRMObjectDescriptor& object = descriptor->objects[description.object_index];

        attributes[index++] = kFdAttribute[plane];
        attributes[index++] = object.fd;
        attributes[index++] = kOffsetAttribute[plane];
        attributes[index++] = static_cast<EGLint>(description.offset);
        attributes[index++] = kPitchAttribute[plane];
        attributes[index++] = static_cast<EGLint>(description.pitch);

        // Modifiers describe the tiling the GPU wrote. Passing them is required
        // on anything modern — an Intel or AMD decoder does not write linear —
        // but the attribute names only exist with the modifiers extension, and
        // a driver without it wants them omitted entirely.
        if (egl_->supportsDmaBufModifiers() && object.format_modifier != 0) {
            attributes[index++] = kModifierLoAttribute[plane];
            attributes[index++] = static_cast<EGLint>(object.format_modifier & 0xFFFFFFFF);
            attributes[index++] = kModifierHiAttribute[plane];
            attributes[index++] = static_cast<EGLint>(object.format_modifier >> 32);
        }
    }
    attributes[index++] = EGL_NONE;

    image_ = egl_->createImage(attributes);
    if (image_ == EGL_NO_IMAGE_KHR) {
        Log::info("eglCreateImageKHR rejected the decoded frame — falling back to upload");
        av.frame_unref(drmFrame_);
        pathKind_ = Path::Upload;
        return false;
    }

    ensureTexture(GL_TEXTURE_EXTERNAL_OES);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_);
    egl_->bindImageToTexture(GL_TEXTURE_EXTERNAL_OES, image_);
    return true;
}

bool VideoSource::uploadFrame(AVFrame* frame) {
    const ffmpeg::Api& av = ffmpeg::api();

    AVFrame* source = frame;
    if (frame->format == AV_PIX_FMT_VAAPI) {
        // Pulling a hardware surface back into system memory. This is the
        // expensive line in the expensive path: it is an uncached read across
        // the PCIe bus on a discrete GPU.
        av.frame_unref(softwareFrame_);
        const int transferred = av.hwframe_transfer_data(softwareFrame_, frame, 0);
        if (transferred < 0) {
            Log::error("cannot read the decoded frame back: " + ffmpeg::errorText(transferred));
            return false;
        }
        source = softwareFrame_;
    }

    if (scaler_ == nullptr) {
        scaler_ = av.sws_get_context(source->width, source->height,
                                     static_cast<AVPixelFormat>(source->format), width_, height_,
                                     AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (scaler_ == nullptr) {
            Log::error("cannot build a colour converter for this frame format");
            return false;
        }
        rgbaBuffer_ = std::make_unique<std::uint8_t[]>(static_cast<size_t>(width_) * height_ * 4);
    }

    std::uint8_t* destination[4] = {rgbaBuffer_.get(), nullptr, nullptr, nullptr};
    int stride[4] = {width_ * 4, 0, 0, 0};
    av.sws_scale(scaler_, source->data, source->linesize, 0, source->height, destination, stride);

    ensureTexture(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgbaBuffer_.get());
    return true;
}

bool VideoSource::render(Surface& surface, FitMode mode) {
    if (!active_ || codec_ == nullptr) return false;

    const int targetWidth = surface.pixelWidth();
    const int targetHeight = surface.pixelHeight();
    if (targetWidth <= 0 || targetHeight <= 0) return false;

    if (!decodeOneFrame()) return false;

    bool drawExternal = false;
    if (pathKind_ == Path::DmaBuf && frame_->format == AV_PIX_FMT_VAAPI) {
        drawExternal = mapFrameAsImage(frame_);
    }
    if (!drawExternal && !uploadFrame(frame_)) return false;

    glViewport(0, 0, targetWidth, targetHeight);
    // Cleared to transparent, not black. In Fit mode the letterbox is whatever
    // the compositor has behind this surface, and a black clear would paint
    // over the user's real wallpaper.
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GlProgram& program = drawExternal ? externalProgram_ : uploadProgram_;
    program.use();
    program.setFit(fitTransform(mode, width_, height_, targetWidth, targetHeight));
    program.setResolution(targetWidth, targetHeight);
    program.setTextureUnit(0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(drawExternal ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, texture_);
    program.drawFullScreen();

    return true;
}

std::string VideoSource::summary() const {
    const char* pathName = "idle";
    switch (pathKind_) {
        case Path::DmaBuf: pathName = "vaapi/dmabuf"; break;
        case Path::Upload: pathName = hwDevice_ != nullptr ? "vaapi/upload" : "software"; break;
        case Path::Idle: break;
    }
    return format("video %dx%d · %d fps · %d-bit · %s", width_, height_, fps_, bitDepth_, pathName);
}

}  // namespace livewall
