#include "record/ffmpeg_locator.h"

#include <algorithm>
#include <cstring>

#include "app_files.h"
#include "files.h"
#include "i18n.h"
#include "record/libav.h"

namespace cap {
namespace {

// Candidates in preference order. Hardware first, because on any machine that
// has one it is both faster and cheaper than x264.
struct Candidate {
  RecordEncoder id;
  const char* ffmpegName;
  const char* label;
  bool hardware;
};

// H.264 first: it plays everywhere. AV1 sits behind it because it needs recent
// hardware on both ends, and H.265 in the middle. The order is what
// RecordEncoder::Auto walks down.
const Candidate kCandidates[] = {
    {RecordEncoder::Nvenc, "h264_nvenc", "H.264 (NVIDIA NVENC)", true},
    {RecordEncoder::QuickSync, "h264_qsv", "H.264 (Intel QuickSync / Arc)", true},
    {RecordEncoder::Amf, "h264_amf", "H.264 (AMD AMF)", true},
    {RecordEncoder::NvencHevc, "hevc_nvenc", "H.265 (NVIDIA NVENC)", true},
    {RecordEncoder::Av1Nvenc, "av1_nvenc", "AV1 (NVIDIA NVENC, RTX 40+)", true},
    {RecordEncoder::Av1QuickSync, "av1_qsv", "AV1 (Intel Arc)", true},
    {RecordEncoder::Av1Amf, "av1_amf", "AV1 (AMD RDNA 3+)", true},
    {RecordEncoder::X264, "libx264", "H.264 (CPU, x264)", false},
    {RecordEncoder::X265, "libx265", "H.265 (CPU, x265)", false},
};

// The other way round: quality per bit first. Nothing here is a judgement about
// which file is better -- only about which of the two costs you care about.
const RecordEncoder kEfficiencyOrder[] = {
    RecordEncoder::Av1Nvenc,  RecordEncoder::Av1QuickSync, RecordEncoder::Av1Amf,
    RecordEncoder::NvencHevc, RecordEncoder::Nvenc,        RecordEncoder::QuickSync,
    RecordEncoder::Amf,       RecordEncoder::X265,         RecordEncoder::X264,
};

// "9.0.1" -> {9, 0, 1}. Stops at the first thing that is not a number.
std::vector<int> NumberParts(const std::string& number) {
  std::vector<int> parts;
  size_t i = 0;
  while (i < number.size() && isdigit((unsigned char)number[i])) {
    int value = 0;
    while (i < number.size() && isdigit((unsigned char)number[i])) {
      value = value * 10 + (number[i] - '0');
      ++i;
    }
    parts.push_back(value);
    if (i < number.size() && number[i] == '.') ++i;
  }
  return parts;
}

}  // namespace

std::string FfmpegReleaseNumber(const std::string& version) {
  // The libraries say "9.0.1-full_build-...", "ffmpeg -version" put "ffmpeg
  // version " in front of the same.
  const char kPrefix[] = "ffmpeg version ";
  const size_t at = version.find(kPrefix);
  size_t from = at == std::string::npos ? 0 : at + sizeof(kPrefix) - 1;
  if (from < version.size() && version[from] == 'n') ++from;
  size_t to = from;
  while (to < version.size() && (isdigit((unsigned char)version[to]) || version[to] == '.')) {
    ++to;
  }
  std::string number = version.substr(from, to - from);
  while (!number.empty() && number.back() == '.') number.pop_back();
  // A git build starts with a date, "2026-03-31-git-...", which would read as
  // the number 2026. A release always has a dot in it; the date does not.
  if (number.empty() || !isdigit((unsigned char)number[0]) ||
      number.find('.') == std::string::npos) {
    return {};
  }
  return number;
}

bool IsNewerFfmpeg(const std::string& remote, const std::string& installed) {
  const std::vector<int> a = NumberParts(remote);
  const std::vector<int> b = NumberParts(installed);
  if (a.empty() || b.empty()) return false;
  for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
    const int x = i < a.size() ? a[i] : 0;
    const int y = i < b.size() ? b[i] : 0;
    if (x != y) return x > y;
  }
  return false;
}

// ------------------------------------------------------------------ locating

std::filesystem::path DefaultRecordFolder(const std::string& name) {
  std::filesystem::path folder = UserVideosFolder();
  if (folder.empty()) folder = ExeFolder();
  return folder / Utf8ToPath(name);
}

std::filesystem::path OwnFfmpegFolder() { return ExeFolder() / "ffmpeg"; }

FfmpegInfo LocateFfmpeg() {
  FfmpegInfo info;
  info.encoders = KnownEncoders();

  // Nothing downloaded is no error, only a reason to offer the download.
  const std::filesystem::path folder = OwnFfmpegFolder();
  if (!Av() && !LibavFilesPresent(folder)) return info;
  const Libav* av = LoadLibav(folder, &info.error);
  if (!av) return info;

  info.found = true;
  info.folder = PathToUtf8(av->folder);
  info.version = av->version;
  info.number = FfmpegReleaseNumber(info.version);
  return info;
}

bool FullChromaCandidate(const std::string& ffmpegName) {
  return ffmpegName == "h264_nvenc" || ffmpegName == "hevc_nvenc" || ffmpegName == "libx264" ||
         ffmpegName == "libx265";
}

void ApplyCachedProbe(FfmpegInfo* info, const std::vector<int>& available, bool fullProbed,
                      const std::vector<int>& full8, const std::vector<int>& full10) {
  if (!info) return;
  if (info->encoders.empty()) info->encoders = KnownEncoders();
  auto has = [](const std::vector<int>& list, RecordEncoder id) {
    return std::find(list.begin(), list.end(), (int)id) != list.end();
  };
  for (EncoderInfo& e : info->encoders) {
    e.tested = true;
    e.available = has(available, e.id);
    e.error.clear();
    e.fullTested = fullProbed;
    e.full8 = fullProbed && e.available && has(full8, e.id);
    e.full10 = e.full8 && has(full10, e.id);
  }
  info->tested = true;
}

std::vector<EncoderInfo> KnownEncoders() {
  std::vector<EncoderInfo> out;
  for (const Candidate& c : kCandidates) {
    EncoderInfo e;
    e.id = c.id;
    e.ffmpegName = c.ffmpegName;
    e.label = c.label;
    e.hardware = c.hardware;
    out.push_back(std::move(e));
  }
  return out;
}

namespace {

// Mid grey in whichever of the test formats `frame` is. The picture does not
// matter, only that every sample is in range.
void FillGrey(AVFrame* frame, const char* pixelFormat) {
  const bool full = std::strstr(pixelFormat, "444") != nullptr;
  const bool wide = std::strstr(pixelFormat, "16") || std::strstr(pixelFormat, "10");
  // yuv444p10le keeps its ten bits at the bottom, the others at the top.
  const uint16_t mid = std::strstr(pixelFormat, "p10le") ? 0x0200 : 0x8000;
  for (int plane = 0; plane < AV_NUM_DATA_POINTERS && frame->data[plane]; ++plane) {
    const int rows = plane == 0 || full ? frame->height : frame->height / 2;
    const size_t bytes = (size_t)frame->linesize[plane] * rows;
    if (!wide) {
      std::memset(frame->data[plane], 128, bytes);
      continue;
    }
    uint16_t* samples = (uint16_t*)frame->data[plane];
    std::fill(samples, samples + bytes / 2, mid);
  }
}

// Takes what the encoder has finished and throws it away. Zero once it wants
// more, or has nothing more after a flush.
int Drain(const Libav& av, AVCodecContext* context, AVPacket* packet) {
  for (;;) {
    const int result = av.avcodec_receive_packet(context, packet);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 0;
    if (result < 0) return result;
    av.av_packet_unref(packet);
  }
}

// A two frame encode in `pixelFormat`, flushed. True when the encoder opened
// and got through it; `error` gets why not.
bool TestEncode(const Libav& av, const std::string& encoder, const char* pixelFormat,
                std::string* error, std::string* logged) {
  LibavQuiet quiet;
  LibavClearError();
  const AVCodec* codec = av.avcodec_find_encoder_by_name(encoder.c_str());
  if (!codec) {
    const Said said = CAP_SAID(T("Nicht in diesem Build", "Not in this build"));
    *error = said.shown;
    *logged = said.logged;
    return false;
  }

  AVCodecContext* context = av.avcodec_alloc_context3(codec);
  AVFrame* frame = av.av_frame_alloc();
  AVPacket* packet = av.av_packet_alloc();
  int result = context && frame && packet ? 0 : AVERROR(ENOMEM);
  if (result >= 0) {
    context->width = 320;
    context->height = 240;
    context->time_base = AVRational{1, 30};
    context->framerate = AVRational{30, 1};
    context->pix_fmt = av.av_get_pix_fmt(pixelFormat);
    result = av.avcodec_open2(context, codec, nullptr);
  }
  if (result >= 0) {
    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    result = av.av_frame_get_buffer(frame, 0);
  }
  if (result >= 0) {
    FillGrey(frame, pixelFormat);
    for (int i = 0; i < 2 && result >= 0; ++i) {
      frame->pts = i;
      result = av.avcodec_send_frame(context, frame);
      if (result >= 0) result = Drain(av, context, packet);
    }
  }
  if (result >= 0) result = av.avcodec_send_frame(context, nullptr);
  if (result >= 0) result = Drain(av, context, packet);

  *error = result >= 0 ? std::string() : LibavError(result);
  *logged = *error;
  av.av_packet_free(&packet);
  av.av_frame_free(&frame);
  av.avcodec_free_context(&context);
  return result >= 0;
}

// One test encode, result cached on the entry. Then 4:4:4 where it can work:
// once more in eight bits, and in ten if that passed, the ten bits in the
// format the recorder hands that encoder (Recorder::FormatFor).
bool TestOne(const Libav& av, EncoderInfo* e) {
  if (e->tested) return e->available;
  std::string logged;
  // What the recorder hands it in 4:2:0: NV12, planar for x265, which takes no
  // NV12 at all.
  const char* format = e->ffmpegName == "libx265" ? "yuv420p" : "nv12";
  e->available = TestEncode(av, e->ffmpegName, format, &e->error, &logged);
  e->tested = true;
  CAP_LOG("Encoder %s: %s%s%s", e->ffmpegName.c_str(), e->available ? "ok" : "not available",
          logged.empty() ? "" : " - ", logged.c_str());

  e->fullTested = true;
  e->full8 = e->full10 = false;
  if (e->available && FullChromaCandidate(e->ffmpegName)) {
    std::string error;
    e->full8 = TestEncode(av, e->ffmpegName, "yuv444p", &error, &logged);
    if (e->full8) {
      const bool nvenc = e->ffmpegName.find("_nvenc") != std::string::npos;
      e->full10 = TestEncode(av, e->ffmpegName, nvenc ? "yuv444p16le" : "yuv444p10le", &error,
                             &logged);
    }
    CAP_LOG("Encoder %s 4:4:4: 8 bit %s, 10 bit %s%s%s", e->ffmpegName.c_str(),
            e->full8 ? "ok" : "no", e->full10 ? "ok" : "no", logged.empty() ? "" : " - ",
            logged.c_str());
  }
  return e->available;
}

}  // namespace

const EncoderInfo* EnsureUsableEncoder(FfmpegInfo* info, RecordEncoder wanted) {
  const Libav* av = Av();
  if (!info || !info->found || !av) return nullptr;

  if (!IsAutoEncoder(wanted)) {
    for (EncoderInfo& e : info->encoders) {
      if (e.id != wanted) continue;
      return TestOne(*av, &e) ? &e : nullptr;
    }
    return nullptr;
  }

  // Preference order, stopping at the first that works. On a machine with a
  // usable GPU that is a single test, so pressing record does not sit there
  // running nine encodes first.
  if (wanted == RecordEncoder::AutoEfficient) {
    for (RecordEncoder id : kEfficiencyOrder) {
      for (EncoderInfo& e : info->encoders) {
        if (e.id == id && TestOne(*av, &e)) return &e;
      }
    }
    return nullptr;
  }
  for (EncoderInfo& e : info->encoders) {
    if (TestOne(*av, &e)) return &e;
  }
  return nullptr;
}

void ProbeEncoders(FfmpegInfo* info) {
  const Libav* av = Av();
  if (!info || !info->found || !av) return;

  // Two frames of grey into nothing. Small enough to be quick, real enough that
  // a missing GPU or a driver mismatch shows up.
  //
  // The pixel format is pinned to what the recorder actually uses. 4:4:4 is
  // its own test: AV1 NVENC on Ada cannot do it (that arrived with Blackwell)
  // and encodes 4:2:0 perfectly well, so a test in 4:4:4 alone would measure
  // 4:4:4 support, not usability.
  //
  // Everything from scratch, results from the configuration included: this is
  // what "Test again" runs, and it must test again.
  info->encoders = KnownEncoders();
  for (EncoderInfo& e : info->encoders) TestOne(*av, &e);
  info->tested = true;
}

// ------------------------------------------------------------------ queries

const EncoderInfo* FfmpegInfo::Find(RecordEncoder id) const {
  for (const EncoderInfo& e : encoders) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

const EncoderInfo* FfmpegInfo::BestAvailable(bool preferEfficiency) const {
  if (preferEfficiency) {
    for (RecordEncoder id : kEfficiencyOrder) {
      for (const EncoderInfo& e : encoders) {
        if (e.id == id && e.available) return &e;
      }
    }
    return nullptr;
  }
  // kCandidates is already in compatibility order, and encoders keeps that order.
  for (const EncoderInfo& e : encoders) {
    if (e.available) return &e;
  }
  return nullptr;
}

const EncoderInfo* FfmpegInfo::Resolve(RecordEncoder wanted) const {
  if (IsAutoEncoder(wanted)) {
    return BestAvailable(wanted == RecordEncoder::AutoEfficient);
  }
  return Find(wanted);
}

bool FfmpegInfo::AnyAvailable() const {
  return BestAvailable(false) != nullptr;
}

}  // namespace cap
