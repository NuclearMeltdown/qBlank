#include "record/ffmpeg_locator.h"

#include <shlobj.h>

#include <algorithm>

#include "app_identity.h"
#include "i18n.h"
#include "text_win32.h"

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

bool FileExists(const std::wstring& path) {
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Searches PATH for ffmpeg.exe.
std::wstring FindOnPath() {
  wchar_t buffer[MAX_PATH * 4] = {};
  wchar_t* filePart = nullptr;
  const DWORD n = ::SearchPathW(nullptr, L"ffmpeg.exe", nullptr, (DWORD)std::size(buffer), buffer,
                                &filePart);
  if (n > 0 && n < std::size(buffer)) return buffer;
  return {};
}

std::string FirstLine(const std::string& text) {
  const size_t end = text.find_first_of("\r\n");
  return end == std::string::npos ? text : text.substr(0, end);
}

}  // namespace

// --------------------------------------------------------------- running it

bool RunFfmpeg(const std::string& exe, const std::string& args, std::string* output,
               DWORD* exitCode, DWORD timeoutMs) {
  if (output) output->clear();
  if (exitCode) *exitCode = (DWORD)-1;
  if (exe.empty()) return false;

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr, writePipe = nullptr;
  if (!::CreatePipe(&readPipe, &writePipe, &sa, 1 << 16)) return false;
  // Only the child may inherit the write end.
  ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  // ffmpeg writes everything interesting to stderr, so both go into one pipe.
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;
  si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  std::wstring commandLine = L"\"" + ToWide(exe) + L"\" " + ToWide(args);
  std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
  mutableCmd.push_back(L'\0');

  PROCESS_INFORMATION pi = {};
  const BOOL ok = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  ::CloseHandle(writePipe);  // the child owns it now
  if (!ok) {
    ::CloseHandle(readPipe);
    return false;
  }

  std::string captured;
  char buffer[4096];
  DWORD read = 0;
  while (::ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    captured.append(buffer, read);
    if (captured.size() > 1 << 20) break;  // a stuck ffmpeg must not eat memory
  }
  ::CloseHandle(readPipe);

  ::WaitForSingleObject(pi.hProcess, timeoutMs);
  DWORD code = 0;
  if (::GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE) {
    ::TerminateProcess(pi.hProcess, 1);
    code = (DWORD)-1;
  }
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);

  if (output) *output = std::move(captured);
  if (exitCode) *exitCode = code;
  return true;
}

// ------------------------------------------------------------------ locating

std::filesystem::path DefaultRecordFolder(const std::string& name) {
  PWSTR videos = nullptr;
  std::wstring folder;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)) && videos) {
    folder = videos;
    ::CoTaskMemFree(videos);
  }
  if (folder.empty()) folder = ExeDirectory();
  if (!folder.empty() && folder.back() != L'\\') folder += L'\\';
  return std::filesystem::path(folder + ToWide(name));
}

FfmpegInfo LocateFfmpeg(const std::string& configuredPath) {
  FfmpegInfo info;
  info.encoders = KnownEncoders();

  std::vector<std::wstring> candidates;
  if (!configuredPath.empty()) candidates.push_back(ToWide(configuredPath));
  const std::wstring exeDir = ExeDirectory();
  candidates.push_back(exeDir + L"ffmpeg\\bin\\ffmpeg.exe");
  candidates.push_back(exeDir + L"ffmpeg\\ffmpeg.exe");
  candidates.push_back(exeDir + L"ffmpeg.exe");

  std::wstring found;
  for (const std::wstring& c : candidates) {
    if (FileExists(c)) {
      found = c;
      break;
    }
  }
  if (found.empty()) found = FindOnPath();
  if (found.empty()) return info;

  // Confirm it actually runs before believing the file name.
  std::string output;
  DWORD exitCode = 0;
  if (!RunFfmpeg(ToUtf8(found), "-hide_banner -version", &output, &exitCode, 8000)) {
    return info;
  }
  if (output.find("ffmpeg version") == std::string::npos) return info;

  info.found = true;
  info.path = ToUtf8(found);
  info.version = FirstLine(output);
  CAP_LOG("ffmpeg found: %s (%s)", info.path.c_str(), info.version.c_str());
  return info;
}

void ApplyCachedProbe(FfmpegInfo* info, const std::vector<int>& available) {
  if (!info) return;
  if (info->encoders.empty()) info->encoders = KnownEncoders();
  for (EncoderInfo& e : info->encoders) {
    e.tested = true;
    e.available = std::find(available.begin(), available.end(), (int)e.id) != available.end();
    e.error.clear();
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

// One test encode, result cached on the entry.
bool TestOne(const std::string& exe, EncoderInfo* e) {
  if (e->tested) return e->available;
  const std::string args =
      "-hide_banner -loglevel error -f lavfi -i testsrc=size=320x240:rate=30 "
      "-frames:v 2 -pix_fmt nv12 -c:v " + e->ffmpegName + " -f null -";

  std::string output;
  DWORD exitCode = 0;
  std::string logged;
  if (!RunFfmpeg(exe, args, &output, &exitCode, 20000)) {
    const Said said = CAP_SAID(T("ffmpeg konnte nicht gestartet werden", "ffmpeg could not be started"));
    e->available = false;
    e->error = said.shown;
    logged = said.logged;
  } else {
    e->available = (exitCode == 0);
    e->error = e->available ? std::string() : FirstLine(Trim(output));
    logged = e->error;
  }
  e->tested = true;
  CAP_LOG("Encoder %s: %s%s%s", e->ffmpegName.c_str(), e->available ? "ok" : "not available",
          logged.empty() ? "" : " - ", logged.c_str());
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
