#include "import/FFmpeg.h"

#include "support/Dynamic.h"
#include "support/Log.h"

namespace livewall {
namespace ffmpeg {
namespace {

// Matched soname sets, newest first. The four libraries version independently,
// and pairing across a major boundary links cleanly and then crashes: AVFrame
// and AVCodecContext both grew fields between these.
//
// Mapping back to releases, since the numbers say nothing on their own:
//   FFmpeg 8  avutil 60, avcodec 62, avformat 62, swscale 9
//   FFmpeg 7  avutil 59, avcodec 61, avformat 61, swscale 8
//   FFmpeg 6  avutil 58, avcodec 60, avformat 60, swscale 7
//   FFmpeg 5  avutil 57, avcodec 59, avformat 59, swscale 6
//   FFmpeg 4  avutil 56, avcodec 58, avformat 58, swscale 5
struct VersionSet {
    const char* release;
    const char* avutil;
    const char* avcodec;
    const char* avformat;
    const char* swscale;
};

constexpr VersionSet kVersionSets[] = {
    {"8", "libavutil.so.60", "libavcodec.so.62", "libavformat.so.62", "libswscale.so.9"},
    {"7", "libavutil.so.59", "libavcodec.so.61", "libavformat.so.61", "libswscale.so.8"},
    {"6", "libavutil.so.58", "libavcodec.so.60", "libavformat.so.60", "libswscale.so.7"},
    {"5", "libavutil.so.57", "libavcodec.so.59", "libavformat.so.59", "libswscale.so.6"},
    {"4", "libavutil.so.56", "libavcodec.so.58", "libavformat.so.58", "libswscale.so.5"},
};

struct State {
    Api api;
    SharedLibrary avutil;
    SharedLibrary avcodec;
    SharedLibrary avformat;
    SharedLibrary swscale;
    bool attempted = false;
    bool ready = false;
};

State& state() {
    static State instance;
    return instance;
}

// Opens one matched set. Each SharedLibrary is fresh per attempt, so a set that
// gets three of four does not leave the fourth's handle behind.
bool openSet(State& s, const VersionSet& set, bool quiet) {
    if (!s.avutil.open("libavutil", {set.avutil}, quiet)) return false;
    if (!s.avcodec.open("libavcodec", {set.avcodec}, quiet)) return false;
    if (!s.avformat.open("libavformat", {set.avformat}, quiet)) return false;
    if (!s.swscale.open("libswscale", {set.swscale}, quiet)) return false;
    return true;
}

void bindAll(State& s) {
    Api& a = s.api;

    s.avutil.bind(a.frame_alloc, "av_frame_alloc");
    s.avutil.bind(a.frame_free, "av_frame_free");
    s.avutil.bind(a.frame_unref, "av_frame_unref");
    s.avutil.bind(a.frame_ref, "av_frame_ref");
    s.avutil.bind(a.frame_get_buffer, "av_frame_get_buffer");
    s.avutil.bind(a.strerror, "av_strerror");
    s.avutil.bind(a.hwdevice_ctx_create, "av_hwdevice_ctx_create");
    s.avutil.bind(a.hwframe_transfer_data, "av_hwframe_transfer_data");
    s.avutil.bind(a.hwframe_ctx_alloc, "av_hwframe_ctx_alloc");
    s.avutil.bind(a.hwframe_ctx_init, "av_hwframe_ctx_init");
    s.avutil.bind(a.hwframe_get_buffer, "av_hwframe_get_buffer");
    s.avutil.bind(a.hwframe_map, "av_hwframe_map");
    s.avutil.bind(a.buffer_ref, "av_buffer_ref");
    s.avutil.bind(a.buffer_unref, "av_buffer_unref");
    s.avutil.bind(a.dict_set, "av_dict_set");
    s.avutil.bind(a.dict_free, "av_dict_free");
    s.avutil.bind(a.log_set_level, "av_log_set_level");
    s.avutil.bind(a.opt_set, "av_opt_set");
    s.avutil.bind(a.rescale_q, "av_rescale_q");
    s.avutil.bind(a.version_info, "av_version_info");
    s.avutil.bind(a.image_get_buffer_size, "av_image_get_buffer_size");
    s.avutil.bind(a.pix_fmt_desc_get, "av_pix_fmt_desc_get");
    s.avutil.bind(a.freep, "av_freep");

    s.avcodec.bind(a.find_decoder, "avcodec_find_decoder");
    s.avcodec.bind(a.find_encoder_by_name, "avcodec_find_encoder_by_name");
    s.avcodec.bind(a.alloc_context, "avcodec_alloc_context3");
    s.avcodec.bind(a.free_context, "avcodec_free_context");
    s.avcodec.bind(a.parameters_to_context, "avcodec_parameters_to_context");
    s.avcodec.bind(a.parameters_from_context, "avcodec_parameters_from_context");
    s.avcodec.bind(a.open2, "avcodec_open2");
    s.avcodec.bind(a.send_packet, "avcodec_send_packet");
    s.avcodec.bind(a.receive_frame, "avcodec_receive_frame");
    s.avcodec.bind(a.send_frame, "avcodec_send_frame");
    s.avcodec.bind(a.receive_packet, "avcodec_receive_packet");
    s.avcodec.bind(a.flush_buffers, "avcodec_flush_buffers");
    s.avcodec.bind(a.get_hw_config, "avcodec_get_hw_config");
    s.avcodec.bind(a.packet_alloc, "av_packet_alloc");
    s.avcodec.bind(a.packet_free, "av_packet_free");
    s.avcodec.bind(a.packet_unref, "av_packet_unref");
    s.avcodec.bind(a.packet_rescale_ts, "av_packet_rescale_ts");

    s.avformat.bind(a.open_input, "avformat_open_input");
    s.avformat.bind(a.close_input, "avformat_close_input");
    s.avformat.bind(a.find_stream_info, "avformat_find_stream_info");
    s.avformat.bind(a.read_frame, "av_read_frame");
    s.avformat.bind(a.seek_frame, "av_seek_frame");
    s.avformat.bind(a.find_best_stream, "av_find_best_stream");
    s.avformat.bind(a.alloc_output_context, "avformat_alloc_output_context2");
    s.avformat.bind(a.new_stream, "avformat_new_stream");
    s.avformat.bind(a.write_header, "avformat_write_header");
    s.avformat.bind(a.interleaved_write_frame, "av_interleaved_write_frame");
    s.avformat.bind(a.write_trailer, "av_write_trailer");
    s.avformat.bind(a.free_format_context, "avformat_free_context");
    s.avformat.bind(a.avio_open, "avio_open");
    s.avformat.bind(a.avio_closep, "avio_closep");

    s.swscale.bind(a.sws_get_context, "sws_getContext");
    s.swscale.bind(a.sws_scale, "sws_scale");
    s.swscale.bind(a.sws_free_context, "sws_freeContext");
}

}  // namespace

bool load() {
    State& s = state();
    if (s.attempted) return s.ready;
    s.attempted = true;

    for (const VersionSet& set : kVersionSets) {
        // Fresh handles per attempt: a set that opens three of four must not
        // leave the successful three bound when the next set is tried.
        State candidate;
        // Quiet: a machine with FFmpeg 6 does not have the FFmpeg 8 or 7
        // sonames, and saying so three times before succeeding reads as three
        // failures rather than as a search.
        if (!openSet(candidate, set, /*quiet=*/true)) continue;

        bindAll(candidate);
        const bool complete = candidate.avutil.complete() && candidate.avcodec.complete() &&
                              candidate.avformat.complete() && candidate.swscale.complete();
        if (!complete) {
            const std::string missing = !candidate.avutil.complete() ? candidate.avutil.missingSymbol()
                                        : !candidate.avcodec.complete()
                                            ? candidate.avcodec.missingSymbol()
                                        : !candidate.avformat.complete()
                                            ? candidate.avformat.missingSymbol()
                                            : candidate.swscale.missingSymbol();
            Log::info(std::string("FFmpeg ") + set.release + " is missing " + missing +
                      " — trying an older set");
            continue;
        }

        // SharedLibrary is non-copyable and holds only a handle and two
        // strings; moving the whole State in would need a move constructor for
        // no benefit, so the successful set is reopened into the real state.
        // dlopen is refcounted and the library is already resident, so this is
        // a refcount bump rather than a second load — and quiet, because it
        // would otherwise log the same four lines twice.
        openSet(s, set, /*quiet=*/true);
        bindAll(s);
        s.ready = true;

        // No av_register_all: it has been a no-op since FFmpeg 4 and was
        // removed in 5. Nothing needs to be initialised.
        //
        // Quiet by default. FFmpeg's default log level writes decoder warnings
        // to stderr, and a wallpaper that prints "co located POCs unavailable"
        // into the journal every time it resumes is not acceptable.
        s.api.log_set_level(Log::verbose() ? AV_LOG_WARNING : AV_LOG_FATAL);

        Log::info("FFmpeg " + versionText() + " (" + set.avcodec + ")");
        return true;
    }

    Log::info("no FFmpeg — video wallpapers are unavailable, procedural mode still works");
    return false;
}

const Api& api() { return state().api; }

std::string versionText() {
    // Loads on demand. Reporting "not available" purely because nothing had
    // called load() yet made `livewall status` contradict the encoder line
    // directly beneath it.
    load();
    const State& s = state();
    if (!s.ready || s.api.version_info == nullptr) return "not available";
    const char* text = s.api.version_info();
    return text != nullptr ? text : "unknown";
}

std::string errorText(int code) {
    const State& s = state();
    if (!s.ready || s.api.strerror == nullptr) return "error " + std::to_string(code);

    char buffer[256] = {};
    if (s.api.strerror(code, buffer, sizeof(buffer)) < 0) {
        return "error " + std::to_string(code);
    }
    return buffer;
}

}  // namespace ffmpeg
}  // namespace livewall
