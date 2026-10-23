#pragma once

// The ffmpeg libraries, inside the process.
//
// Nothing links against them. The program starts without them and records once
// the downloader has put them in place: libavutil, libswresample, libavcodec
// and libavformat, opened by full path from that one folder. Their functions
// are looked up into one table, and their major versions are held against the
// headers in third_party/ffmpeg -- another major is another ABI, and is refused
// rather than called into.
//
// Loaded once and never unloaded. A recording, a probe and a remux can all be
// running in them at the same time.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

namespace cap {

// Every function the program calls, by library, in the order they are opened:
// each one needs the ones before it.
#define QB_AVUTIL_FUNCTIONS(X)                                                                   \
  X(avutil_version) X(av_version_info) X(av_log_set_callback) X(av_log_format_line2)            \
  X(av_strerror) X(av_frame_alloc) X(av_frame_free) X(av_frame_ref) X(av_frame_unref)           \
  X(av_frame_get_buffer) X(av_frame_make_writable) X(av_buffer_pool_init) X(av_buffer_pool_get) \
  X(av_buffer_pool_uninit) X(av_buffer_allocz) X(av_buffer_ref) X(av_buffer_unref)              \
  X(av_get_pix_fmt) X(av_d2q) X(av_rescale_q) X(av_dict_set) X(av_dict_get) X(av_dict_copy)     \
  X(av_dict_free) X(av_opt_set_int) X(av_channel_layout_default) X(av_channel_layout_uninit)    \
  X(av_frame_move_ref) X(av_hwdevice_ctx_alloc) X(av_hwdevice_ctx_init) X(av_hwframe_ctx_alloc) \
  X(av_hwframe_ctx_init) X(av_hwframe_get_buffer) X(av_hwframe_transfer_data)                   \
  X(av_hwdevice_ctx_create) X(av_opt_find)
#define QB_SWRESAMPLE_FUNCTIONS(X)                                                 \
  X(swresample_version) X(swr_alloc_set_opts2) X(swr_init) X(swr_convert)            \
  X(swr_set_compensation) X(swr_get_delay) X(swr_get_out_samples) X(swr_free)
#define QB_AVCODEC_FUNCTIONS(X)                                                                  \
  X(avcodec_version) X(avcodec_find_encoder_by_name) X(avcodec_alloc_context3) X(avcodec_open2) \
  X(avcodec_get_hw_config) X(avcodec_send_frame) X(avcodec_receive_packet)                      \
  X(avcodec_free_context)                                                                       \
  X(avcodec_parameters_from_context) X(avcodec_parameters_copy) X(av_packet_alloc)              \
  X(av_packet_free) X(av_packet_unref) X(av_packet_rescale_ts) X(av_packet_ref)               \
  X(av_packet_clone)
#define QB_AVFORMAT_FUNCTIONS(X)                                                                 \
  X(avformat_version) X(avformat_alloc_context) X(avformat_alloc_output_context2)              \
  X(avformat_new_stream) X(avformat_query_codec) X(avio_open) X(avio_open2) X(avio_closep)      \
  X(avio_size) X(avformat_write_header) X(av_interleaved_write_frame) X(av_write_trailer)       \
  X(avformat_free_context) X(avformat_open_input) X(avformat_find_stream_info)                  \
  X(avformat_close_input) X(av_read_frame)

struct Libav {
#define QB_AV_FIELD(name) decltype(&::name) name = nullptr;
  QB_AVUTIL_FUNCTIONS(QB_AV_FIELD)
  QB_SWRESAMPLE_FUNCTIONS(QB_AV_FIELD)
  QB_AVCODEC_FUNCTIONS(QB_AV_FIELD)
  QB_AVFORMAT_FUNCTIONS(QB_AV_FIELD)
#undef QB_AV_FIELD

  // What the build calls itself, "9.0.2-full_build-www.gyan.dev".
  std::string version;
  std::filesystem::path folder;
};

// The four file names, in the order they are opened: "avutil-61.dll" and so on.
// The majors come from the headers, so they are also what the downloader takes
// out of the archive.
std::vector<std::string> LibavFileNames();

// True when all four files are in `folder`. Says nothing about whether they load.
bool LibavFilesPresent(const std::filesystem::path& folder);

// Opens the libraries in `folder`, once. Later calls hand back the same table
// whatever folder they name. A failed attempt leaves nothing open and can be
// repeated, after a download for instance. Null when a file is missing or does
// not load, a function is not in it, or a major differs from the headers, with
// `error` saying which.
const Libav* LoadLibav(const std::filesystem::path& folder, std::string* error);

// The table LoadLibav filled, null before one succeeded.
const Libav* Av();

// What went wrong in ffmpeg's own words: the first error it logged on this
// thread since LibavClearError, or else the text of `code`.
std::string LibavError(int code);
void LibavClearError();

// Keeps ffmpeg's errors on this thread out of the log for as long as it lives:
// in the probes a failure is an answer, not a fault. LibavError still has them.
class LibavQuiet {
 public:
  LibavQuiet();
  ~LibavQuiet();
  LibavQuiet(const LibavQuiet&) = delete;
  LibavQuiet& operator=(const LibavQuiet&) = delete;

 private:
  bool was_;
};

}  // namespace cap
