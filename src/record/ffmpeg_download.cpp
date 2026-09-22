#include "record/ffmpeg_download.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <vector>

#include "app_files.h"
#include "child_process.h"
#include "http.h"
#include "i18n.h"
#include "text_win32.h"

namespace cap {
namespace {

// gyan.dev "release-essentials": static, linked from ffmpeg.org, stable URL that
// redirects to whatever the current release is. Measured against the BtbN build
// at 106 vs 163 MB with the same set of encoders, so this is the smaller of two
// equals.
const char kArchiveUrl[] = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";
const char kHashUrl[] =
    "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip.sha256";

// A download of this size needs longer than the default to get going.
HttpRequest Ask(const char* url) {
  HttpRequest request;
  request.url = url;
  request.userAgent = AppNameUtf8();
  request.timeoutMs = 30000;
  return request;
}

// The stable URL answers with a redirect whose target carries the version:
// .../packages/ffmpeg-9.0.1-essentials_build.zip
std::string VersionFromRedirect() {
  HttpRequest request = Ask(kArchiveUrl);
  request.followRedirects = false;  // the redirect is the answer
  HttpResponse response;
  if (!HttpGet(request, &response, nullptr, nullptr)) return {};
  const std::string& text = response.location;

  const size_t start = text.find("ffmpeg-");
  if (start == std::string::npos) return {};
  const size_t from = start + 7;
  size_t to = from;
  while (to < text.size() && (isdigit((unsigned char)text[to]) || text[to] == '.')) ++to;
  if (to == from) return {};
  std::string version = text.substr(from, to - from);
  while (!version.empty() && version.back() == '.') version.pop_back();
  return version;
}

std::string DownloadText(const char* url) {
  std::string out;
  if (!HttpGetString(Ask(url), &out, nullptr, nullptr, 4096)) return {};
  return out;
}

std::string HexSha256(const std::wstring& file) {
  HANDLE f = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return {};

  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string result;
  std::vector<uint8_t> hashObject;
  std::vector<uint8_t> digest;

  do {
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) break;
    DWORD objLen = 0, hashLen = 0, cb = 0;
    if (::BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0) != 0) break;
    if (::BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0) != 0) break;
    hashObject.resize(objLen);
    digest.resize(hashLen);
    if (::BCryptCreateHash(alg, &hash, hashObject.data(), objLen, nullptr, 0, 0) != 0) break;

    std::vector<uint8_t> buffer(1 << 20);
    DWORD read = 0;
    while (::ReadFile(f, buffer.data(), (DWORD)buffer.size(), &read, nullptr) && read > 0) {
      if (::BCryptHashData(hash, buffer.data(), read, 0) != 0) break;
    }
    if (::BCryptFinishHash(hash, digest.data(), hashLen, 0) != 0) break;

    char hex[3];
    for (uint8_t b : digest) {
      std::snprintf(hex, sizeof(hex), "%02x", b);
      result += hex;
    }
  } while (false);

  if (hash) ::BCryptDestroyHash(hash);
  if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
  ::CloseHandle(f);
  return result;
}

// Windows 10 1803 and later ship bsdtar as tar.exe, and it reads ZIP. That
// saves carrying a zip library for one button.
bool ExtractWithTar(const std::wstring& archive, const std::wstring& intoFolder,
                    std::string* error) {
  wchar_t system32[MAX_PATH] = {};
  if (::GetSystemDirectoryW(system32, MAX_PATH) == 0) return false;
  const std::wstring tar = std::wstring(system32) + L"\\tar.exe";
  if (::GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES) {
    ReportError(error, CAP_SAID(T("tar.exe fehlt (Windows 10 1803 oder neuer nötig).",
                                  "tar.exe is missing (needs Windows 10 1803 or newer).")));
    return false;
  }

  // Pull out only the one member, dropping its folders. The pattern is quoted in
  // a shell to keep the shell from expanding it; as one argument among others it
  // reaches tar unchanged either way.
  ProcessSpec spec;
  spec.program = ToUtf8(tar);
  spec.Add("-xf", ToUtf8(archive));
  spec.Add("-C", ToUtf8(intoFolder));
  spec.Add("--strip-components=2");
  spec.Add("*/bin/ffmpeg.exe");

  int code = 1;
  if (!RunAndWait(spec, &code, 120000)) {
    ReportError(error, CAP_SAID(T("tar.exe ließ sich nicht starten.", "Could not start tar.exe.")));
    return false;
  }
  if (code != 0) {
    return ReportError(error, CAP_SAID(Format(T("Entpacken fehlgeschlagen (tar %lu).", "Extracting failed (tar %lu)."),
                                              (unsigned long)code)));
  }
  return true;
}

}  // namespace

FfmpegDownloader::~FfmpegDownloader() {
  Cancel();
  if (thread_.joinable()) thread_.join();
}

void FfmpegDownloader::Cancel() {
  cancel_.store(true, std::memory_order_relaxed);
}

std::string FfmpegDownloader::message() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return message_;
}

std::string FfmpegDownloader::remoteVersion() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return remoteVersion_;
}

std::string FfmpegDownloader::resultPath() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return resultPath_;
}

void FfmpegDownloader::SetMessage(const std::string& text) {
  std::lock_guard<std::mutex> lock(mutex_);
  message_ = text;
}

bool FfmpegDownloader::Start(const std::filesystem::path& targetFolder) {
  if (busy()) return false;
  if (thread_.joinable()) thread_.join();
  cancel_.store(false, std::memory_order_relaxed);
  progress_.store(-1.0f, std::memory_order_relaxed);
  state_.store(State::Running, std::memory_order_relaxed);
  thread_ = std::thread(&FfmpegDownloader::Run, this, targetFolder, false);
  return true;
}

bool FfmpegDownloader::StartVersionCheck() {
  if (busy()) return false;
  if (thread_.joinable()) thread_.join();
  cancel_.store(false, std::memory_order_relaxed);
  state_.store(State::Running, std::memory_order_relaxed);
  thread_ = std::thread(&FfmpegDownloader::Run, this, std::filesystem::path(), true);
  return true;
}

void FfmpegDownloader::Run(std::filesystem::path targetFolder, bool versionOnly) {
  auto finish = [&](bool ok, const Said& said) {
    if (!ok && !said.logged.empty()) CAP_ERR("ffmpeg download: %s", said.logged.c_str());
    SetMessage(said.shown);
    state_.store(ok ? State::Done : State::Failed, std::memory_order_relaxed);
  };

  SetMessage(T("Frage Version ab ...", "Checking the version ..."));
  const std::string version = VersionFromRedirect();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remoteVersion_ = version;
  }
  if (versionOnly) {
    if (version.empty()) {
      finish(false, CAP_SAID(T("Version konnte nicht ermittelt werden.", "Could not determine the version.")));
    } else {
      finish(true, CAP_SAID(T("Neueste Version: ", "Latest version: ") + version));
    }
    return;
  }

  // --- the checksum first, so a mismatch is detectable at all ---
  SetMessage(T("Hole Prüfsumme ...", "Fetching the checksum ..."));
  std::string expected = Trim(DownloadText(kHashUrl));
  const size_t space = expected.find_first_of(" \t");
  if (space != std::string::npos) expected = expected.substr(0, space);
  expected = ToUpper(expected);
  for (char& c : expected) c = (char)tolower((unsigned char)c);

  // --- download ---
  SetMessage(T("Lade ffmpeg herunter ...", "Downloading ffmpeg ..."));
  wchar_t tempDir[MAX_PATH] = {};
  ::GetTempPathW(MAX_PATH, tempDir);
  const std::wstring archive = std::wstring(tempDir) + L"qblank_ffmpeg.zip";

  {
    HANDLE out = ::CreateFileW(archive.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
      finish(false, CAP_SAID(T("Temporäre Datei konnte nicht angelegt werden.",
                               "Could not create the temporary file.")));
      return;
    }

    uint64_t received = 0;
    HttpResponse response;
    // Straight to the file as it arrives: a hundred megabytes has no business
    // sitting in memory first. Answering false is how Cancel gets out of here.
    const bool ok = HttpGet(Ask(kArchiveUrl), &response,
                            [&](const void* data, size_t size) {
                              if (cancel_.load(std::memory_order_relaxed)) return false;
                              DWORD written = 0;
                              if (!::WriteFile(out, data, (DWORD)size, &written, nullptr) ||
                                  written != size) {
                                return false;
                              }
                              received += size;
                              if (response.contentLength > 0) {
                                progress_.store(
                                    (float)((double)received / (double)response.contentLength));
                              }
                              SetMessage(
                                  Format(T("Lade ffmpeg ... %.1f MB", "Downloading ffmpeg ... %.1f MB"),
                                         (double)received / (1024.0 * 1024.0)));
                              return true;
                            },
                            nullptr);
    ::CloseHandle(out);

    if (!ok || received == 0) {
      ::DeleteFileW(archive.c_str());
      finish(false, CAP_SAID(cancel_.load(std::memory_order_relaxed)
                                 ? T("Abgebrochen.", "Cancelled.")
                                 : T("Download fehlgeschlagen.", "The download failed.")));
      return;
    }
  }

  // --- verify ---
  if (!expected.empty()) {
    SetMessage(T("Prüfe SHA-256 ...", "Verifying SHA-256 ..."));
    const std::string actual = HexSha256(archive);
    if (actual.empty() || actual != expected) {
      ::DeleteFileW(archive.c_str());
      finish(false, CAP_SAID(T("Prüfsumme stimmt nicht — Download verworfen.",
                               "Checksum mismatch — the download was discarded.")));
      return;
    }
  } else {
    CAP_WARN("ffmpeg download: no checksum to be had, extracting unchecked");
  }

  // --- extract ---
  SetMessage(T("Entpacke ...", "Extracting ..."));
  ::CreateDirectoryW(targetFolder.c_str(), nullptr);
  std::string extractError;
  if (!ExtractWithTar(archive, targetFolder.native(), &extractError)) {
    ::DeleteFileW(archive.c_str());
    finish(false, Relayed(extractError));
    return;
  }
  ::DeleteFileW(archive.c_str());

  const std::wstring exe = targetFolder.native() + L"\\ffmpeg.exe";
  if (::GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    finish(false, CAP_SAID(T("ffmpeg.exe war nicht im Archiv.", "ffmpeg.exe was not in the archive.")));
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    resultPath_ = ToUtf8(exe);
  }
  progress_.store(1.0f);
  CAP_LOG("ffmpeg downloaded: %s (version %s)", ToUtf8(exe).c_str(), version.c_str());
  finish(true, CAP_SAID(T("ffmpeg ist bereit.", "ffmpeg is ready.")));
}

}  // namespace cap
