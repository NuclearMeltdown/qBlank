// The part of screenshot.h that is the same everywhere: AVIF through ffmpeg,
// and where the files go. The encoders Windows ships are in screenshot_win32.cpp.

#include "record/screenshot.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <vector>

#include "app_files.h"
#include "files.h"
#include "i18n.h"
#include "record/libav.h"

namespace cap {

namespace {

// One IEEE half float, widened. Exact: every half has a float that is equal
// to it.
float HalfToFloat(uint16_t h) {
  const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1fu;
  const uint32_t mantissa = h & 0x3ffu;
  if (exponent == 0) {
    // Zero and the subnormals: the mantissa in units of 2^-24.
    const float f = (float)mantissa * (1.0f / 16777216.0f);
    return sign ? -f : f;
  }
  const uint32_t bits = exponent == 31 ? sign | 0x7f800000u | (mantissa << 13)
                                       : sign | ((exponent + 112) << 23) | (mantissa << 13);
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

float Pq(float nits) {
  const float m1 = 0.1593017578125f, m2 = 78.84375f;
  const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
  const float yv = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
  const float p = std::pow(yv, m1);
  return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

uint16_t Code10(float value) { return (uint16_t)std::clamp((int)std::lround(value), 0, 1023); }

// Linear light with 1.0 at diffuse white, onto the PQ curve in BT.2020 and into
// ten bit Y'CbCr at TV range, chroma averaged over each two by two block. The
// same arithmetic the shader does for the recording path, in software because
// a still is one frame.
void HalfToPq420(const uint16_t* halfRgba, int width, int height, int stride,
                 float paperWhiteNits, AVFrame* frame) {
  const int chromaWidth = (width + 1) / 2;
  std::vector<float> cb((size_t)chromaWidth), cr((size_t)chromaWidth);
  for (int y = 0; y < height; ++y) {
    const uint16_t* src = halfRgba + (size_t)y * ((size_t)stride / sizeof(uint16_t));
    uint16_t* luma = (uint16_t*)(frame->data[0] + (size_t)y * frame->linesize[0]);
    if (y % 2 == 0) {
      std::fill(cb.begin(), cb.end(), 0.0f);
      std::fill(cr.begin(), cr.end(), 0.0f);
    }
    for (int x = 0; x < width; ++x) {
      float rgb[3];
      for (int c = 0; c < 3; ++c) {
        rgb[c] = std::max(HalfToFloat(src[(size_t)x * 4 + c]), 0.0f) * paperWhiteNits;
      }
      const float r = Pq(0.627404f * rgb[0] + 0.329283f * rgb[1] + 0.043313f * rgb[2]);
      const float g = Pq(0.069097f * rgb[0] + 0.919540f * rgb[1] + 0.011362f * rgb[2]);
      const float b = Pq(0.016391f * rgb[0] + 0.088013f * rgb[1] + 0.895595f * rgb[2]);
      const float yp = 0.2627f * r + 0.6780f * g + 0.0593f * b;
      luma[x] = Code10(64.0f + 876.0f * yp);
      cb[(size_t)x / 2] += (b - yp) / 1.8814f;
      cr[(size_t)x / 2] += (r - yp) / 1.4746f;
    }
    // A block is written once its second row is in, or the picture ends.
    if (y % 2 == 1 || y == height - 1) {
      const int rows = y % 2 + 1;
      uint16_t* u = (uint16_t*)(frame->data[1] + (size_t)(y / 2) * frame->linesize[1]);
      uint16_t* v = (uint16_t*)(frame->data[2] + (size_t)(y / 2) * frame->linesize[2]);
      for (int x = 0; x < chromaWidth; ++x) {
        const int columns = std::min(2, width - x * 2);
        const float scale = 896.0f / (float)(rows * columns);
        u[x] = Code10(512.0f + cb[(size_t)x] * scale);
        v[x] = Code10(512.0f + cr[(size_t)x] * scale);
      }
    }
  }
}

}  // namespace

bool SaveScreenshotAvif(const std::filesystem::path& path, const uint16_t* halfRgba, int width,
                        int height, int stride, float paperWhiteNits, std::string* error) {
  auto fail = [&](const Said& said) {
    CAP_ERR("AVIF screenshot: %s", said.logged.c_str());
    if (error) *error = said.shown;
    return false;
  };
  if (!halfRgba || width <= 0 || height <= 0) return fail(CAP_SAID(T("Kein Bild.", "No picture.")));
  const Libav* loaded = Av();
  if (!loaded) {
    return fail(CAP_SAID(T("AVIF braucht ffmpeg. Unter Aufnahme herunterladen, oder JPEG XR wählen.",
                           "AVIF needs ffmpeg. Download it under Recording, or choose JPEG XR.")));
  }
  const Libav& av = *loaded;
  const AVCodec* codec = av.avcodec_find_encoder_by_name("libaom-av1");
  if (!codec) {
    return fail(CAP_SAID(T("Diesem ffmpeg fehlt libaom-av1.", "This ffmpeg lacks libaom-av1.")));
  }

  LibavClearError();
  AVCodecContext* encoder = av.avcodec_alloc_context3(codec);
  AVFrame* frame = av.av_frame_alloc();
  AVPacket* packet = av.av_packet_alloc();
  AVFormatContext* out = nullptr;
  bool fileOpen = false;
  bool written = false;
  auto finish = [&](const Said* said) {
    av.av_packet_free(&packet);
    av.av_frame_free(&frame);
    av.avcodec_free_context(&encoder);
    if (out) {
      if (fileOpen) av.avio_closep(&out->pb);
      av.avformat_free_context(out);
    }
    if (!said) return true;
    if (fileOpen && !written) RemoveFile(path);
    return fail(*said);
  };
  auto failed = [&](int code) {
    const Said said = CAP_SAID(Format(T("Das AVIF ließ sich nicht schreiben: %s",
                                        "The AVIF could not be written: %s"),
                                      LibavError(code).c_str()));
    return finish(&said);
  };
  if (!encoder || !frame || !packet) return failed(AVERROR(ENOMEM));

  const std::string file = PathToUtf8(path);
  int result = av.avformat_alloc_output_context2(&out, nullptr, "avif", file.c_str());
  if (result < 0) return failed(result);

  // Without the description it is a dark picture rather than an HDR one;
  // nothing else in the file says the samples are on the PQ curve.
  encoder->width = width;
  encoder->height = height;
  encoder->pix_fmt = AV_PIX_FMT_YUV420P10LE;
  encoder->time_base = {1, 1};
  encoder->framerate = {1, 1};
  encoder->color_primaries = AVCOL_PRI_BT2020;
  encoder->color_trc = AVCOL_TRC_SMPTE2084;
  encoder->colorspace = AVCOL_SPC_BT2020_NCL;
  encoder->color_range = AVCOL_RANGE_MPEG;
  if (out->oformat->flags & AVFMT_GLOBALHEADER) encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  AVDictionary* options = nullptr;
  av.av_dict_set(&options, "still-picture", "1", 0);
  av.av_dict_set(&options, "crf", "18", 0);
  result = av.avcodec_open2(encoder, codec, &options);
  av.av_dict_free(&options);
  if (result < 0) return failed(result);

  frame->format = encoder->pix_fmt;
  frame->width = width;
  frame->height = height;
  frame->color_primaries = encoder->color_primaries;
  frame->color_trc = encoder->color_trc;
  frame->colorspace = encoder->colorspace;
  frame->color_range = encoder->color_range;
  frame->pts = 0;
  if ((result = av.av_frame_get_buffer(frame, 0)) < 0) return failed(result);
  HalfToPq420(halfRgba, width, height, stride, paperWhiteNits, frame);

  AVStream* stream = av.avformat_new_stream(out, nullptr);
  if (!stream) return failed(AVERROR(ENOMEM));
  if ((result = av.avcodec_parameters_from_context(stream->codecpar, encoder)) < 0) {
    return failed(result);
  }
  stream->time_base = encoder->time_base;
  if ((result = av.avio_open(&out->pb, file.c_str(), AVIO_FLAG_WRITE)) < 0) return failed(result);
  fileOpen = true;
  if ((result = av.avformat_write_header(out, nullptr)) < 0) return failed(result);

  // One frame in, then the flush: libaom hands the picture over only once it
  // knows nothing follows.
  if ((result = av.avcodec_send_frame(encoder, frame)) < 0) return failed(result);
  if ((result = av.avcodec_send_frame(encoder, nullptr)) < 0) return failed(result);
  while ((result = av.avcodec_receive_packet(encoder, packet)) >= 0) {
    av.av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
    packet->stream_index = stream->index;
    if ((result = av.av_interleaved_write_frame(out, packet)) < 0) return failed(result);
  }
  if (result != AVERROR_EOF) return failed(result);
  if ((result = av.av_write_trailer(out)) < 0) return failed(result);
  written = true;
  return finish(nullptr);
}

std::filesystem::path DefaultScreenshotFolder(const std::string& name) {
  std::filesystem::path folder = UserPicturesFolder();
  if (folder.empty()) folder = ExeFolder();
  return folder / Utf8ToPath(name);
}

std::filesystem::path MakeHdrScreenshotPath(const std::filesystem::path& folder,
                                            HdrShotFormat format) {
  std::filesystem::path base = MakeScreenshotPath(folder, ScreenshotFormat::Png);
  if (base.empty()) return base;
  base.replace_extension(format == HdrShotFormat::Avif ? ".avif" : ".jxr");
  return base;
}

std::filesystem::path MakeScreenshotPath(const std::filesystem::path& folder,
                                         ScreenshotFormat format) {
  if (!EnsureFolder(folder)) return {};

  const std::string ext = format == ScreenshotFormat::Jpeg ? ".jpg" : ".png";
  const LocalTime now = NowLocal();
  const std::string stamp =
      Format("%s_%04d-%02d-%02d_%02d-%02d-%02d", AppNameUtf8().c_str(), now.year, now.month,
             now.day, now.hour, now.minute, now.second);

  // Someone holding the key down produces several shots inside one second, and
  // silently overwriting the earlier ones would be the wrong answer.
  std::filesystem::path candidate = folder / Utf8ToPath(stamp + ext);
  for (int n = 2; IsFile(candidate) && n < 1000; ++n) {
    candidate = folder / Utf8ToPath(stamp + "_" + std::to_string(n) + ext);
  }
  return candidate;
}

}  // namespace cap
