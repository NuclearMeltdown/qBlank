#include "record/ffmpeg_download.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

#include "app_identity.h"
#include "i18n.h"
#include "text_win32.h"

namespace cap {
namespace {

// gyan.dev "release-essentials": static, linked from ffmpeg.org, stable URL that
// redirects to whatever the current release is. Measured against the BtbN build
// at 106 vs 163 MB with the same set of encoders, so this is the smaller of two
// equals.
const wchar_t kHost[] = L"www.gyan.dev";
const wchar_t kPath[] = L"/ffmpeg/builds/ffmpeg-release-essentials.zip";
const wchar_t kHashPath[] = L"/ffmpeg/builds/ffmpeg-release-essentials.zip.sha256";

struct WinHttpHandles {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;
  HINTERNET request = nullptr;
  ~WinHttpHandles() {
    if (request) ::WinHttpCloseHandle(request);
    if (connect) ::WinHttpCloseHandle(connect);
    if (session) ::WinHttpCloseHandle(session);
  }
};

bool OpenRequest(WinHttpHandles* h, const wchar_t* path, bool followRedirects) {
  h->session = ::WinHttpOpen(kAppName, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!h->session) return false;

  DWORD timeout = 30000;
  ::WinHttpSetTimeouts(h->session, timeout, timeout, timeout, timeout);

  h->connect = ::WinHttpConnect(h->session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!h->connect) return false;

  h->request = ::WinHttpOpenRequest(h->connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!h->request) return false;

  if (!followRedirects) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    ::WinHttpSetOption(h->request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
  }
  if (!::WinHttpSendRequest(h->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0)) {
    return false;
  }
  return ::WinHttpReceiveResponse(h->request, nullptr) != FALSE;
}

std::wstring ReadHeader(HINTERNET request, DWORD header) {
  DWORD size = 0;
  ::WinHttpQueryHeaders(request, header, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (!::WinHttpQueryHeaders(request, header, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size,
                             WINHTTP_NO_HEADER_INDEX)) {
    return {};
  }
  value.resize(wcslen(value.c_str()));
  return value;
}

// The stable URL answers with a redirect whose target carries the version:
// .../packages/ffmpeg-9.0.1-essentials_build.zip
std::string VersionFromRedirect() {
  WinHttpHandles h;
  if (!OpenRequest(&h, kPath, false)) return {};
  const std::wstring location = ReadHeader(h.request, WINHTTP_QUERY_LOCATION);
  const std::string text = ToUtf8(location);

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

std::string DownloadText(const wchar_t* path) {
  WinHttpHandles h;
  if (!OpenRequest(&h, path, true)) return {};
  std::string out;
  DWORD available = 0;
  while (::WinHttpQueryDataAvailable(h.request, &available) && available > 0) {
    std::vector<char> buffer(available);
    DWORD read = 0;
    if (!::WinHttpReadData(h.request, buffer.data(), available, &read) || read == 0) break;
    out.append(buffer.data(), read);
    if (out.size() > 4096) break;
  }
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

  // Pull out only the one member, dropping its folders.
  std::wstring cmd = L"\"" + tar + L"\" -xf \"" + archive + L"\" -C \"" + intoFolder +
                     L"\" --strip-components=2 \"*/bin/ffmpeg.exe\"";
  std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
  mutableCmd.push_back(L'\0');

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};
  if (!::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
    ReportError(error, CAP_SAID(T("tar.exe ließ sich nicht starten.", "Could not start tar.exe.")));
    return false;
  }
  ::WaitForSingleObject(pi.hProcess, 120000);
  DWORD code = 1;
  ::GetExitCodeProcess(pi.hProcess, &code);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
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
  std::string expected = Trim(DownloadText(kHashPath));
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
    WinHttpHandles h;
    if (!OpenRequest(&h, kPath, true)) {
      finish(false, CAP_SAID(T("Verbindung fehlgeschlagen.", "The connection failed.")));
      return;
    }
    const std::wstring lengthText = ReadHeader(h.request, WINHTTP_QUERY_CONTENT_LENGTH);
    const uint64_t total = lengthText.empty() ? 0 : _wcstoui64(lengthText.c_str(), nullptr, 10);

    HANDLE out = ::CreateFileW(archive.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
      finish(false, CAP_SAID(T("Temporäre Datei konnte nicht angelegt werden.",
                               "Could not create the temporary file.")));
      return;
    }

    uint64_t received = 0;
    DWORD available = 0;
    bool ok = true;
    while (::WinHttpQueryDataAvailable(h.request, &available) && available > 0) {
      if (cancel_.load(std::memory_order_relaxed)) {
        ok = false;
        break;
      }
      std::vector<char> buffer(available);
      DWORD read = 0;
      if (!::WinHttpReadData(h.request, buffer.data(), available, &read) || read == 0) break;
      DWORD written = 0;
      if (!::WriteFile(out, buffer.data(), read, &written, nullptr) || written != read) {
        ok = false;
        break;
      }
      received += read;
      if (total > 0) progress_.store((float)((double)received / (double)total));
      SetMessage(Format(T("Lade ffmpeg ... %.1f MB", "Downloading ffmpeg ... %.1f MB"),
                        (double)received / (1024.0 * 1024.0)));
    }
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
