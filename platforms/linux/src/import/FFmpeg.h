// libavformat / libavcodec / libavutil / libswscale, resolved at runtime.
//
// This is the file that answers "what replaces AVFoundation and Media
// Foundation on Linux", and the answer has a caveat the other two ports do not
// need. There is no media framework the OS guarantees. FFmpeg is on
// essentially every desktop install — it is what the video player, the browser
// and the screen recorder are all already using — but it is a package, not a
// platform, and a machine can be missing it.
//
// So it is opened with dlopen and everything downstream of it is optional. No
// ffmpeg means no video wallpapers; the procedural mode still runs, `livewall
// status` says why, and the app does not fail to start. Linking it instead
// would have made a missing package into a binary that will not launch.
//
// Two things this loader does that a naive dlopen would get wrong:
//
//   Version sets, not individual sonames. The four libraries are versioned
//   independently, and a machine mid-upgrade can have libavcodec.so.61 and
//   libavutil.so.58 side by side. Opening each by its own list would pair a
//   codec from FFmpeg 7 with a utility library from FFmpeg 6, which links and
//   then crashes on the first AVFrame — the structure grew fields between them.
//   The table below is a list of *matched sets*, tried newest first, and a set
//   is abandoned whole if any of its four is absent.
//
//   Every symbol or none. `load()` returns false if any binding failed, and it
//   names the missing one. A partially bound table would crash at the first
//   call rather than at startup.
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <string>

namespace livewall {
namespace ffmpeg {

struct Api {
    // --- libavutil ---
    decltype(::av_frame_alloc)* frame_alloc = nullptr;
    decltype(::av_frame_free)* frame_free = nullptr;
    decltype(::av_frame_unref)* frame_unref = nullptr;
    decltype(::av_frame_ref)* frame_ref = nullptr;
    decltype(::av_frame_get_buffer)* frame_get_buffer = nullptr;
    decltype(::av_strerror)* strerror = nullptr;
    decltype(::av_hwdevice_ctx_create)* hwdevice_ctx_create = nullptr;
    decltype(::av_hwframe_transfer_data)* hwframe_transfer_data = nullptr;
    decltype(::av_hwframe_ctx_alloc)* hwframe_ctx_alloc = nullptr;
    decltype(::av_hwframe_ctx_init)* hwframe_ctx_init = nullptr;
    decltype(::av_hwframe_get_buffer)* hwframe_get_buffer = nullptr;
    decltype(::av_hwframe_map)* hwframe_map = nullptr;
    decltype(::av_buffer_ref)* buffer_ref = nullptr;
    decltype(::av_buffer_unref)* buffer_unref = nullptr;
    decltype(::av_dict_set)* dict_set = nullptr;
    decltype(::av_dict_free)* dict_free = nullptr;
    decltype(::av_log_set_level)* log_set_level = nullptr;
    decltype(::av_opt_set)* opt_set = nullptr;
    decltype(::av_rescale_q)* rescale_q = nullptr;
    decltype(::av_version_info)* version_info = nullptr;
    decltype(::av_image_get_buffer_size)* image_get_buffer_size = nullptr;
    decltype(::av_pix_fmt_desc_get)* pix_fmt_desc_get = nullptr;
    decltype(::av_freep)* freep = nullptr;

    // --- libavcodec ---
    decltype(::avcodec_find_decoder)* find_decoder = nullptr;
    decltype(::avcodec_find_encoder_by_name)* find_encoder_by_name = nullptr;
    decltype(::avcodec_alloc_context3)* alloc_context = nullptr;
    decltype(::avcodec_free_context)* free_context = nullptr;
    decltype(::avcodec_parameters_to_context)* parameters_to_context = nullptr;
    decltype(::avcodec_parameters_from_context)* parameters_from_context = nullptr;
    decltype(::avcodec_open2)* open2 = nullptr;
    decltype(::avcodec_send_packet)* send_packet = nullptr;
    decltype(::avcodec_receive_frame)* receive_frame = nullptr;
    decltype(::avcodec_send_frame)* send_frame = nullptr;
    decltype(::avcodec_receive_packet)* receive_packet = nullptr;
    decltype(::avcodec_flush_buffers)* flush_buffers = nullptr;
    decltype(::avcodec_get_hw_config)* get_hw_config = nullptr;
    decltype(::av_packet_alloc)* packet_alloc = nullptr;
    decltype(::av_packet_free)* packet_free = nullptr;
    decltype(::av_packet_unref)* packet_unref = nullptr;
    decltype(::av_packet_rescale_ts)* packet_rescale_ts = nullptr;

    // --- libavformat ---
    decltype(::avformat_open_input)* open_input = nullptr;
    decltype(::avformat_close_input)* close_input = nullptr;
    decltype(::avformat_find_stream_info)* find_stream_info = nullptr;
    decltype(::av_read_frame)* read_frame = nullptr;
    decltype(::av_seek_frame)* seek_frame = nullptr;
    decltype(::av_find_best_stream)* find_best_stream = nullptr;
    decltype(::avformat_alloc_output_context2)* alloc_output_context = nullptr;
    decltype(::avformat_new_stream)* new_stream = nullptr;
    decltype(::avformat_write_header)* write_header = nullptr;
    decltype(::av_interleaved_write_frame)* interleaved_write_frame = nullptr;
    decltype(::av_write_trailer)* write_trailer = nullptr;
    decltype(::avformat_free_context)* free_format_context = nullptr;
    decltype(::avio_open)* avio_open = nullptr;
    decltype(::avio_closep)* avio_closep = nullptr;

    // --- libswscale ---
    decltype(::sws_getContext)* sws_get_context = nullptr;
    decltype(::sws_scale)* sws_scale = nullptr;
    decltype(::sws_freeContext)* sws_free_context = nullptr;
};

// Idempotent. Safe to call from anywhere; the first call does the work.
bool load();

// Only valid when load() returned true.
const Api& api();

// The FFmpeg build string, or "not available".
std::string versionText();

// "Invalid data found when processing input" from an AVERROR. FFmpeg's codes
// are errno values negated *or* four-character tags, and the bare number tells
// nobody anything.
std::string errorText(int code);

}  // namespace ffmpeg
}  // namespace livewall
