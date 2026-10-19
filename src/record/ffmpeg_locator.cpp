#include "record/ffmpeg_locator.h"

#include <algorithm>

#include "app_files.h"
#include "child_process.h"
#include "files.h"
#include "i18n.h"

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

std::string FirstLine(const std::string& text) {
  const size_t end = text.find_first_of("\r\n");
  return end == std::string::npos ? text : text.substr(0, end);
}

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

std::string FfmpegReleaseNumber(const std::string& versionLine) {
  const char kPrefix[] = "ffmpeg version ";
  const size_t at = versionLine.find(kPrefix);
  if (at == std::string::npos) return {};
  size_t from = at + sizeof(kPrefix) - 1;
  if (from < versionLine.size() && versionLine[from] == 'n') ++from;
  size_t to = from;
  while (to < versionLine.size() &&
         (isdigit((unsigned char)versionLine[to]) || versionLine[to] == '.')) {
    ++to;
  }
  std::string number = versionLine.substr(from, to - from);
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

bool IsMajorFfmpegStep(const std::string& remote, const std::string& installed) {
  const std::vector<int> a = NumberParts(remote);
  const std::vector<int> b = NumberParts(installed);
  return !a.empty() && !b.empty() && a[0] != b[0];
}


// ------------------------------------------------------------------ locating

std::filesystem::path DefaultRecordFolder(const std::string& name) {
  std::filesystem::path folder = UserVideosFolder();
  if (folder.empty()) folder = ExeFolder();
  return folder / Utf8ToPath(name);
}

FfmpegInfo LocateFfmpeg(const std::string& configuredPath) {
  FfmpegInfo info;
  info.encoders = KnownEncoders();

  std::vector<std::filesystem::path> candidates;
  if (!configuredPath.empty()) candidates.push_back(Utf8ToPath(configuredPath));
  const std::filesystem::path exeDir = ExeFolder();
  candidates.push_back(exeDir / "ffmpeg" / "bin" / "ffmpeg.exe");
  candidates.push_back(exeDir / "ffmpeg" / "ffmpeg.exe");
  candidates.push_back(exeDir / "ffmpeg.exe");

  std::filesystem::path found;
  for (const std::filesystem::path& c : candidates) {
    if (IsFile(c)) {
      found = c;
      break;
    }
  }
  if (found.empty()) found = ProgramOnPath("ffmpeg");
  if (found.empty()) return info;

  // Confirm it actually runs before believing the file name.
  ProcessSpec spec;
  spec.program = PathToUtf8(found);
  spec.Add("-hide_banner", "-version");

  std::string output;
  if (!RunAndCollect(spec, &output, nullptr, 8000)) return info;
  if (output.find("ffmpeg version") == std::string::npos) return info;

  info.found = true;
  info.path = PathToUtf8(found);
  info.version = FirstLine(output);
  info.number = FfmpegReleaseNumber(info.version);
  CAP_LOG("ffmpeg found: %s (%s)", info.path.c_str(), info.version.c_str());
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

// A two frame encode in `pixelFormat`. True when ffmpeg ran and exited
// cleanly; `error` gets why not.
bool TestEncode(const std::string& exe, const std::string& encoder, const char* pixelFormat,
                std::string* error, std::string* logged) {
  ProcessSpec spec;
  spec.program = exe;
  spec.Add("-hide_banner");
  spec.Add("-loglevel", "error");
  spec.Add("-f", "lavfi");
  spec.Add("-i", "testsrc=size=320x240:rate=30");
  spec.Add("-frames:v", "2");
  spec.Add("-pix_fmt", pixelFormat);
  spec.Add("-c:v", encoder);
  spec.Add("-f", "null");
  spec.Add("-");

  std::string output;
  int exitCode = 0;
  if (!RunAndCollect(spec, &output, &exitCode, 20000)) {
    const Said said = CAP_SAID(T("ffmpeg konnte nicht gestartet werden", "ffmpeg could not be started"));
    *error = said.shown;
    *logged = said.logged;
    return false;
  }
  *error = exitCode == 0 ? std::string() : FirstLine(Trim(output));
  *logged = *error;
  return exitCode == 0;
}

// One test encode, result cached on the entry. Then 4:4:4 where it can work:
// once more in eight bits, and in ten if that passed, the ten bits in the
// format the recorder hands that encoder (Recorder::FormatFor).
bool TestOne(const std::string& exe, EncoderInfo* e) {
  if (e->tested) return e->available;
  std::string logged;
  e->available = TestEncode(exe, e->ffmpegName, "nv12", &e->error, &logged);
  e->tested = true;
  CAP_LOG("Encoder %s: %s%s%s", e->ffmpegName.c_str(), e->available ? "ok" : "not available",
          logged.empty() ? "" : " - ", logged.c_str());

  e->fullTested = true;
  e->full8 = e->full10 = false;
  if (e->available && FullChromaCandidate(e->ffmpegName)) {
    std::string error;
    e->full8 = TestEncode(exe, e->ffmpegName, "yuv444p", &error, &logged);
    if (e->full8) {
      const bool nvenc = e->ffmpegName.find("_nvenc") != std::string::npos;
      e->full10 = TestEncode(exe, e->ffmpegName, nvenc ? "yuv444p16le" : "yuv444p10le", &error,
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
  if (!info || !info->found) return nullptr;

  if (!IsAutoEncoder(wanted)) {
    for (EncoderInfo& e : info->encoders) {
      if (e.id != wanted) continue;
      return TestOne(info->path, &e) ? &e : nullptr;
    }
    return nullptr;
  }

  // Preference order, stopping at the first that works. On a machine with a
  // usable GPU that is a single test, so pressing record does not sit there
  // running nine encodes first.
  if (wanted == RecordEncoder::AutoEfficient) {
    for (RecordEncoder id : kEfficiencyOrder) {
      for (EncoderInfo& e : info->encoders) {
        if (e.id == id && TestOne(info->path, &e)) return &e;
      }
    }
    return nullptr;
  }
  for (EncoderInfo& e : info->encoders) {
    if (TestOne(info->path, &e)) return &e;
  }
  return nullptr;
}

void ProbeEncoders(FfmpegInfo* info) {
  if (!info || !info->found) return;

  // A two frame encode of a colour bar pattern into nothing. Small enough to be
  // quick, real enough that a missing GPU or a driver mismatch shows up.
  //
  // The pixel format must be pinned to what the recorder actually uses. Left to
  // itself, ffmpeg converts the RGB test pattern to gbrp -- planar RGB, i.e.
  // 4:4:4 -- because that is the closest lossless match in the encoder's format
  // list. AV1 NVENC on Ada cannot do 4:4:4 (that arrived with Blackwell), so the
  // test failed with "No capable devices found" on hardware that encodes AV1
  // perfectly well. It was measuring 4:4:4 support, not usability.
  //
  // Everything from scratch, results from the configuration included: this is
  // what "Test again" runs, and it must test again.
  info->encoders = KnownEncoders();
  for (EncoderInfo& e : info->encoders) TestOne(info->path, &e);
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
