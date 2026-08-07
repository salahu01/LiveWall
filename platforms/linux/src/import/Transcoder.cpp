#include "import/Transcoder.h"

#include <algorithm>
#include <cmath>

#include "import/CodecSupport.h"
#include "import/FFmpeg.h"
#include "import/FramePacer.h"
#include "support/Log.h"
#include "support/Paths.h"
#include "support/Strings.h"

namespace livewall {
namespace {

// HEVC and H.264 4:2:0 both require even dimensions.
int makeEven(double value) {
    int rounded = static_cast<int>(std::lround(value));
    rounded -= rounded % 2;
    return std::max(2, rounded);
}

// Everything the conversion loop owns, so that every exit path releases the
// same set. With -fno-exceptions there is no unwinding to lean on and a
// half-dozen `goto cleanup` labels is the alternative.
struct Pipeline {
    AVFormatContext* input = nullptr;
    AVFormatContext* output = nullptr;
    AVCodecContext* decoder = nullptr;
    AVCodecContext* encoder = nullptr;
    AVBufferRef* hwDevice = nullptr;
    AVBufferRef* hwFrames = nullptr;
    AVPacket* packet = nullptr;
    AVPacket* encoded = nullptr;
    AVFrame* decoded = nullptr;
    AVFrame* converted = nullptr;
    AVFrame* hardware = nullptr;
    SwsContext* scaler = nullptr;

    ~Pipeline() {
        if (!ffmpeg::load()) return;
        const ffmpeg::Api& av = ffmpeg::api();

        if (scaler != nullptr) av.sws_free_context(scaler);
        if (decoded != nullptr) av.frame_free(&decoded);
        if (converted != nullptr) av.frame_free(&converted);
        if (hardware != nullptr) av.frame_free(&hardware);
        if (packet != nullptr) av.packet_free(&packet);
        if (encoded != nullptr) av.packet_free(&encoded);
        // Contexts before the buffers they reference.
        if (decoder != nullptr) av.free_context(&decoder);
        if (encoder != nullptr) av.free_context(&encoder);
        if (hwFrames != nullptr) av.buffer_unref(&hwFrames);
        if (hwDevice != nullptr) av.buffer_unref(&hwDevice);
        if (input != nullptr) av.close_input(&input);
        if (output != nullptr) {
            if ((output->oformat->flags & AVFMT_NOFILE) == 0 && output->pb != nullptr) {
                av.avio_closep(&output->pb);
            }
            av.free_format_context(output);
        }
    }
};

bool setUpHardwareFrames(Pipeline& pipeline, const EncoderChoice& choice, int width, int height) {
    const ffmpeg::Api& av = ffmpeg::api();

    int result = av.hwdevice_ctx_create(&pipeline.hwDevice, AV_HWDEVICE_TYPE_VAAPI,
                                        "/dev/dri/renderD128", nullptr, 0);
    if (result < 0) {
        Log::error("no VA-API device for " + choice.name + ": " + ffmpeg::errorText(result));
        return false;
    }

    pipeline.hwFrames = av.hwframe_ctx_alloc(pipeline.hwDevice);
    if (pipeline.hwFrames == nullptr) return false;

    auto* frames = reinterpret_cast<AVHWFramesContext*>(pipeline.hwFrames->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = static_cast<AVPixelFormat>(choice.pixelFormat);
    frames->width = width;
    frames->height = height;
    // Small on purpose. Frames are uploaded and encoded one at a time; a large
    // pool would be surfaces the size of the output sitting idle for the whole
    // import.
    frames->initial_pool_size = 4;

    result = av.hwframe_ctx_init(pipeline.hwFrames);
    if (result < 0) {
        Log::error("cannot initialise the VA-API frame pool: " + ffmpeg::errorText(result));
        return false;
    }
    return true;
}

// Drains whatever the encoder has ready and muxes it. Called after every frame
// and again with a null frame to flush.
bool drainEncoder(Pipeline& pipeline, AVStream* stream, bool* failed) {
    const ffmpeg::Api& av = ffmpeg::api();

    for (;;) {
        const int result = av.receive_packet(pipeline.encoder, pipeline.encoded);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
        if (result < 0) {
            Log::error("encoder failed: " + ffmpeg::errorText(result));
            *failed = true;
            return false;
        }

        pipeline.encoded->stream_index = stream->index;
        // The encoder times in 1/fps; the muxer wants the stream's own base,
        // which for MP4 is usually 1/90000 or 1/fps depending on what the muxer
        // chose in write_header.
        av.packet_rescale_ts(pipeline.encoded, pipeline.encoder->time_base, stream->time_base);

        const int written = av.interleaved_write_frame(pipeline.output, pipeline.encoded);
        av.packet_unref(pipeline.encoded);
        if (written < 0) {
            Log::error("cannot write to the output file: " + ffmpeg::errorText(written));
            *failed = true;
            return false;
        }
    }
}

// Hands one converted frame to the encoder, uploading it first when the encoder
// lives on the GPU.
bool submitFrame(Pipeline& pipeline, AVFrame* frame) {
    const ffmpeg::Api& av = ffmpeg::api();

    if (pipeline.hwFrames == nullptr) return av.send_frame(pipeline.encoder, frame) >= 0;

    av.frame_unref(pipeline.hardware);
    int result = av.hwframe_get_buffer(pipeline.hwFrames, pipeline.hardware, 0);
    if (result < 0) {
        Log::error("no VA-API surface available: " + ffmpeg::errorText(result));
        return false;
    }

    result = av.hwframe_transfer_data(pipeline.hardware, frame, 0);
    if (result < 0) {
        Log::error("cannot upload a frame to the GPU: " + ffmpeg::errorText(result));
        return false;
    }
    pipeline.hardware->pts = frame->pts;
    return av.send_frame(pipeline.encoder, pipeline.hardware) >= 0;
}

}  // namespace

std::string TranscodePreset::summary() const {
    const std::string size = maxEdge > 0 ? std::to_string(maxEdge) + "p" : "your display";
    return format("%s · %d fps · %d-bit", size.c_str(), fps, bitDepth);
}

const std::vector<TranscodePreset>& Transcoder::presets() {
    // Identical to the macOS and Windows presets, deliberately. A library
    // imported on one platform is playable on another, and a user who knows
    // what "Balanced" means on one machine should not find it means something
    // else on the next.
    static const std::vector<TranscodePreset> kPresets = {
        {"Ultra Light", 960, 20, 0.10, 8, 5},
        {"Balanced", 1920, 24, 0.15, 8, 5},
        // 24 rather than 30: frame rate is the one knob that costs CPU linearly,
        // it divides a 120 Hz panel as evenly as 30 does, and ambient loops gain
        // nothing from the extra six frames.
        {"Native", 0, 24, 0.12, 10, 5},
    };
    return kPresets;
}

const TranscodePreset& Transcoder::presetNamed(std::string_view name) {
    for (const TranscodePreset& preset : presets()) {
        if (equalsIgnoreCase(preset.name, name)) return preset;
    }
    // "Fidelity" was the old top preset on macOS. Anyone who chose it wanted
    // the best available, which is now "Native".
    if (equalsIgnoreCase(name, "fidelity") || equalsIgnoreCase(name, "native")) {
        return presets()[2];
    }
    if (equalsIgnoreCase(name, "ultra")) return presets()[0];
    return defaultPreset();
}

const TranscodePreset& Transcoder::defaultPreset() { return presets()[1]; }

void Transcoder::outputSize(int sourceWidth, int sourceHeight, const TranscodePreset& preset,
                            const DisplayTarget& display, int* width, int* height) {
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        const double edge = preset.maxEdge > 0 ? preset.maxEdge : display.pixelWidth;
        *width = makeEven(edge);
        *height = makeEven(edge * 9.0 / 16.0);
        return;
    }

    double scale = 1.0;
    if (preset.maxEdge > 0) {
        const double longest = std::max(sourceWidth, sourceHeight);
        if (longest > preset.maxEdge) scale = preset.maxEdge / longest;
    } else {
        const double cover = std::max(static_cast<double>(display.pixelWidth) / sourceWidth,
                                      static_cast<double>(display.pixelHeight) / sourceHeight);
        scale = std::min(cover, 1.0);
    }

    *width = makeEven(sourceWidth * scale);
    *height = makeEven(sourceHeight * scale);
}

int Transcoder::pacedFps(int preferred, int refresh) {
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

std::optional<TranscodeResult> Transcoder::convert(const std::string& source,
                                                   const std::string& destination,
                                                   const TranscodePreset& preset,
                                                   const DisplayTarget& display,
                                                   const ProgressFn& progress) {
    if (!ffmpeg::load()) {
        Log::error("no FFmpeg, so nothing can be imported");
        return std::nullopt;
    }
    const ffmpeg::Api& av = ffmpeg::api();

    Pipeline pipeline;

    int result = av.open_input(&pipeline.input, source.c_str(), nullptr, nullptr);
    if (result < 0) {
        Log::error("cannot read " + paths::filename(source) + ": " + ffmpeg::errorText(result) +
                   ". It may be missing, damaged, or not a video file.");
        return std::nullopt;
    }
    if (av.find_stream_info(pipeline.input, nullptr) < 0) {
        Log::error("no stream information in " + paths::filename(source));
        return std::nullopt;
    }

    const AVCodec* decoderCodec = nullptr;
    const int streamIndex =
        av.find_best_stream(pipeline.input, AVMEDIA_TYPE_VIDEO, -1, -1, &decoderCodec, 0);
    if (streamIndex < 0 || decoderCodec == nullptr) {
        Log::error("that file has no video track");
        return std::nullopt;
    }

    AVStream* inputStream = pipeline.input->streams[streamIndex];

    pipeline.decoder = av.alloc_context(decoderCodec);
    if (pipeline.decoder == nullptr) return std::nullopt;
    if (av.parameters_to_context(pipeline.decoder, inputStream->codecpar) < 0) return std::nullopt;
    // Import is a batch job with nothing waiting on latency, so unlike the
    // playback decoder this one is allowed all the threads it wants.
    pipeline.decoder->thread_count = 0;
    if (av.open2(pipeline.decoder, decoderCodec, nullptr) < 0) {
        Log::error("no decoder for that file's codec");
        return std::nullopt;
    }

    const int sourceWidth = pipeline.decoder->width;
    const int sourceHeight = pipeline.decoder->height;
    if (sourceWidth <= 0 || sourceHeight <= 0) return std::nullopt;

    int outputWidth = 0;
    int outputHeight = 0;
    outputSize(sourceWidth, sourceHeight, preset, display, &outputWidth, &outputHeight);

    // Never upsample the frame rate past the source, then land on a rate the
    // display can present evenly.
    int sourceFps = 0;
    if (inputStream->avg_frame_rate.den > 0 && inputStream->avg_frame_rate.num > 0) {
        sourceFps = static_cast<int>(std::lround(static_cast<double>(inputStream->avg_frame_rate.num) /
                                                 inputStream->avg_frame_rate.den));
    }
    const int capped = sourceFps > 0 ? std::min(preset.fps, sourceFps) : preset.fps;
    const int fps = std::max(1, pacedFps(capped, display.refreshHz > 0 ? display.refreshHz : 60));
    if (fps != capped) {
        Log::info(format("paced %d fps down to %d to divide a %d Hz display evenly", capped, fps,
                         display.refreshHz));
    }

    const std::optional<EncoderChoice> choice = CodecSupport::chooseEncoder(preset.bitDepth);
    if (!choice.has_value()) return std::nullopt;
    if (choice->maxBitDepth < preset.bitDepth) {
        Log::info(format("%s cannot do %d-bit here — encoding %d-bit", choice->name.c_str(),
                         preset.bitDepth, choice->maxBitDepth));
    }

    const AVCodec* encoderCodec = av.find_encoder_by_name(choice->name.c_str());
    if (encoderCodec == nullptr) return std::nullopt;

    paths::removeFile(destination);

    result = av.alloc_output_context(&pipeline.output, nullptr, nullptr, destination.c_str());
    if (result < 0 || pipeline.output == nullptr) {
        Log::error("cannot write " + destination + ": " + ffmpeg::errorText(result));
        return std::nullopt;
    }

    pipeline.encoder = av.alloc_context(encoderCodec);
    if (pipeline.encoder == nullptr) return std::nullopt;

    pipeline.encoder->width = outputWidth;
    pipeline.encoder->height = outputHeight;
    pipeline.encoder->time_base = AVRational{1, fps};
    pipeline.encoder->framerate = AVRational{fps, 1};
    pipeline.encoder->bit_rate = static_cast<std::int64_t>(
        static_cast<double>(outputWidth) * outputHeight * fps * preset.bitsPerPixel);
    // Sparse keyframes: at these bitrates an I-frame every two seconds ate a
    // large share of the budget the moving content wanted. The only seek a
    // wallpaper performs is the resume after occlusion, which tolerates a
    // longer run-up.
    pipeline.encoder->gop_size = static_cast<int>(fps * preset.keyframeSeconds);
    // No B-frames: the decoder needs no reorder buffer at playback.
    pipeline.encoder->max_b_frames = 0;

    if (choice->hardware) {
        if (!setUpHardwareFrames(pipeline, *choice, outputWidth, outputHeight)) return std::nullopt;
        pipeline.encoder->pix_fmt = AV_PIX_FMT_VAAPI;
        pipeline.encoder->hw_frames_ctx = av.buffer_ref(pipeline.hwFrames);
    } else {
        pipeline.encoder->pix_fmt = static_cast<AVPixelFormat>(choice->pixelFormat);
    }

    if ((pipeline.output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        pipeline.encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // x264 and x265 both log to stderr at their default verbosity and both are
    // chatty enough to bury the app's own output. `preset` is the speed/size
    // trade; medium is where the curve flattens for this content.
    if (startsWith(choice->name, "libx26")) {
        av.opt_set(pipeline.encoder->priv_data, "preset", "medium", 0);
        av.opt_set(pipeline.encoder->priv_data,
                   choice->name == "libx265" ? "x265-params" : "x264-params", "log-level=none", 0);
    }

    result = av.open2(pipeline.encoder, encoderCodec, nullptr);
    if (result < 0) {
        Log::error("cannot set up " + choice->name + ": " + ffmpeg::errorText(result));
        return std::nullopt;
    }

    AVStream* outputStream = av.new_stream(pipeline.output, nullptr);
    if (outputStream == nullptr) return std::nullopt;
    if (av.parameters_from_context(outputStream->codecpar, pipeline.encoder) < 0) {
        return std::nullopt;
    }
    outputStream->time_base = pipeline.encoder->time_base;

    if ((pipeline.output->oformat->flags & AVFMT_NOFILE) == 0) {
        result = av.avio_open(&pipeline.output->pb, destination.c_str(), AVIO_FLAG_WRITE);
        if (result < 0) {
            Log::error("cannot create " + destination + ": " + ffmpeg::errorText(result));
            return std::nullopt;
        }
    }

    AVDictionary* muxerOptions = nullptr;
    av.dict_set(&muxerOptions, "movflags", "+faststart", 0);
    result = av.write_header(pipeline.output, &muxerOptions);
    av.dict_free(&muxerOptions);
    if (result < 0) {
        Log::error("cannot write the file header: " + ffmpeg::errorText(result));
        return std::nullopt;
    }

    pipeline.packet = av.packet_alloc();
    pipeline.encoded = av.packet_alloc();
    pipeline.decoded = av.frame_alloc();
    pipeline.converted = av.frame_alloc();
    pipeline.hardware = av.frame_alloc();
    if (pipeline.packet == nullptr || pipeline.encoded == nullptr || pipeline.decoded == nullptr ||
        pipeline.converted == nullptr || pipeline.hardware == nullptr) {
        return std::nullopt;
    }

    pipeline.converted->format = choice->pixelFormat;
    pipeline.converted->width = outputWidth;
    pipeline.converted->height = outputHeight;
    if (av.frame_get_buffer(pipeline.converted, 0) < 0) {
        Log::error("cannot allocate the conversion buffer");
        return std::nullopt;
    }

    const double totalSeconds =
        pipeline.input->duration > 0 ? static_cast<double>(pipeline.input->duration) / AV_TIME_BASE
                                     : 0.0;

    FramePacer pacer(fps);
    bool failed = false;
    bool endOfInput = false;

    while (!endOfInput && !failed) {
        const int read = av.read_frame(pipeline.input, pipeline.packet);
        if (read == AVERROR_EOF) {
            endOfInput = true;
            av.send_packet(pipeline.decoder, nullptr);  // flush the decoder
        } else if (read < 0) {
            Log::error("read failed: " + ffmpeg::errorText(read));
            failed = true;
            break;
        } else if (pipeline.packet->stream_index != streamIndex) {
            av.packet_unref(pipeline.packet);
            continue;
        } else {
            av.send_packet(pipeline.decoder, pipeline.packet);
            av.packet_unref(pipeline.packet);
        }

        for (;;) {
            const int got = av.receive_frame(pipeline.decoder, pipeline.decoded);
            if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) break;
            if (got < 0) {
                Log::error("decode failed: " + ffmpeg::errorText(got));
                failed = true;
                break;
            }

            const std::int64_t pts = pipeline.decoded->best_effort_timestamp != AV_NOPTS_VALUE
                                         ? pipeline.decoded->best_effort_timestamp
                                         : pipeline.decoded->pts;
            const std::int64_t microseconds =
                pts == AV_NOPTS_VALUE
                    ? 0
                    : av.rescale_q(pts, inputStream->time_base, AVRational{1, 1000000});

            std::int64_t outputIndex = 0;
            if (!pacer.accept(microseconds, &outputIndex)) {
                av.frame_unref(pipeline.decoded);
                continue;
            }

            if (pipeline.scaler == nullptr) {
                pipeline.scaler = av.sws_get_context(
                    pipeline.decoded->width, pipeline.decoded->height,
                    static_cast<AVPixelFormat>(pipeline.decoded->format), outputWidth, outputHeight,
                    static_cast<AVPixelFormat>(choice->pixelFormat),
                    // Lanczos, not bilinear. This runs once per imported file
                    // and the result is looked at for months; bilinear
                    // downscaling of a 4K source to 1080p visibly softens it.
                    SWS_LANCZOS, nullptr, nullptr, nullptr);
                if (pipeline.scaler == nullptr) {
                    Log::error("cannot build a scaler for this source format");
                    failed = true;
                    break;
                }
            }

            av.sws_scale(pipeline.scaler, pipeline.decoded->data, pipeline.decoded->linesize, 0,
                         pipeline.decoded->height, pipeline.converted->data,
                         pipeline.converted->linesize);
            pipeline.converted->pts = outputIndex;

            if (!submitFrame(pipeline, pipeline.converted)) {
                failed = true;
                break;
            }
            if (!drainEncoder(pipeline, outputStream, &failed)) break;

            if (progress && totalSeconds > 0) {
                const double fraction = static_cast<double>(microseconds) / 1000000.0 / totalSeconds;
                progress(std::clamp(fraction, 0.0, 1.0));
            }

            av.frame_unref(pipeline.decoded);
        }
    }

    if (!failed) {
        av.send_frame(pipeline.encoder, nullptr);  // flush the encoder
        drainEncoder(pipeline, outputStream, &failed);
    }

    if (failed) {
        paths::removeFile(destination);
        return std::nullopt;
    }

    if (av.write_trailer(pipeline.output) < 0) {
        Log::error("cannot finalise " + destination);
        paths::removeFile(destination);
        return std::nullopt;
    }

    if (progress) progress(1.0);

    Log::info(format("kept %d frames, dropped %d", pacer.kept(), pacer.dropped()));

    TranscodeResult converted;
    converted.path = destination;
    converted.width = outputWidth;
    converted.height = outputHeight;
    converted.fps = fps;
    converted.bitDepth = choice->maxBitDepth;
    converted.codec = choice->codec;
    // Read after the trailer: +faststart rewrites the file, so the size before
    // that point is not the size on disk.
    converted.byteCount = paths::fileSize(destination);
    return converted;
}

}  // namespace livewall
