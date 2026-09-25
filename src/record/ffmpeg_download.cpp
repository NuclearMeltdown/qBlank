#include "record/ffmpeg_download.h"

#include "app_files.h"
#include "archive.h"
#include "files.h"
#include "http.h"
#include "i18n.h"
#include "record/ffmpeg_locator.h"

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

}  // namespace

std::filesystem::path OwnFfmpegFolder() { return ExeFolder() / "ffmpeg"; }

bool IsOwnFfmpeg(const std::string& path) {
  return !path.empty() && Utf8ToPath(path) == OwnFfmpegFolder() / "ffmpeg.exe";
}

void FfmpegDownloader::CleanUp(const std::filesystem::path& targetFolder) {
  const std::filesystem::path old = targetFolder / "ffmpeg.exe.old";
  if (IsFile(old)) RemoveFile(old);
  const std::filesystem::path staging = targetFolder / "new";
  if (PathExists(staging)) {
    RemoveFile(staging / "ffmpeg.exe");
    RemoveFile(staging);  // takes an empty folder as well
  }
}

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

std::string FfmpegDownloader::installedVersion() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return installedVersion_;
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

bool FfmpegDownloader::StartVersionCheck(const std::string& installed, bool announce) {
  if (busy()) return false;
  if (thread_.joinable()) thread_.join();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    installedVersion_ = installed;
  }
  announce_.store(announce, std::memory_order_relaxed);
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
      return;
    }
    const std::string installed = installedVersion();
    const bool newer = IsNewerFfmpeg(version, installed);
    updateAvailable_.store(newer, std::memory_order_relaxed);
    if (newer) {
      CAP_LOG("ffmpeg update available: %s (installed %s)", version.c_str(), installed.c_str());
      finish(true, CAP_SAID(Format(T("ffmpeg %s ist verfügbar, installiert ist %s.",
                                     "ffmpeg %s is available, %s is installed."),
                                   version.c_str(), installed.c_str())));
    } else if (!installed.empty()) {
      finish(true, CAP_SAID(Format(T("Aktuell (neueste ist %s).", "Up to date (newest is %s)."),
                                   version.c_str())));
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
  // Lower case, as Sha256HexOfFile writes it.
  for (char& c : expected) c = (char)tolower((unsigned char)c);

  // --- download ---
  SetMessage(T("Lade ffmpeg herunter ...", "Downloading ffmpeg ..."));
  const std::filesystem::path archive = TempFolder() / "qblank_ffmpeg.zip";

  {
    FileWriter out;
    if (!out.Open(archive)) {
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
                              if (!out.Write(data, size)) return false;
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
    const bool written = out.Close();

    if (!ok || !written || received == 0) {
      RemoveFile(archive);
      finish(false, CAP_SAID(cancel_.load(std::memory_order_relaxed)
                                 ? T("Abgebrochen.", "Cancelled.")
                                 : T("Download fehlgeschlagen.", "The download failed.")));
      return;
    }
  }

  // --- verify ---
  if (!expected.empty()) {
    SetMessage(T("Prüfe SHA-256 ...", "Verifying SHA-256 ..."));
    const std::string actual = Sha256HexOfFile(archive);
    if (actual.empty() || actual != expected) {
      RemoveFile(archive);
      finish(false, CAP_SAID(T("Prüfsumme stimmt nicht — Download verworfen.",
                               "Checksum mismatch — the download was discarded.")));
      return;
    }
  } else {
    CAP_WARN("ffmpeg download: no checksum to be had, extracting unchecked");
  }

  // --- extract ---
  // Into a folder of its own first, and only then over the old one. ffmpeg.exe
  // may be running -- a recording, a remux -- and a running program cannot be
  // written over. It can be renamed, though: the old one steps aside, the new
  // one takes its name, and the old file goes once nothing has it open.
  SetMessage(T("Entpacke ...", "Extracting ..."));
  CleanUp(targetFolder);
  const std::filesystem::path staging = targetFolder / "new";
  EnsureFolder(staging);
  std::string extractError;
  // Two folders deep in every one of these archives: ffmpeg-<version>/bin/.
  if (!ExtractFromZip(archive, "*/bin/ffmpeg.exe", 2, staging, &extractError)) {
    RemoveFile(archive);
    CleanUp(targetFolder);
    finish(false, Relayed(extractError));
    return;
  }
  RemoveFile(archive);

  const std::filesystem::path fresh = staging / "ffmpeg.exe";
  if (!IsFile(fresh)) {
    CleanUp(targetFolder);
    finish(false, CAP_SAID(T("ffmpeg.exe war nicht im Archiv.", "ffmpeg.exe was not in the archive.")));
    return;
  }
  const std::filesystem::path exe = targetFolder / "ffmpeg.exe";
  const std::filesystem::path old = targetFolder / "ffmpeg.exe.old";
  const bool replacing = IsFile(exe);
  if (replacing && !RenameOver(exe, old)) {
    CleanUp(targetFolder);
    finish(false, CAP_SAID(T("Das bisherige ffmpeg.exe ließ sich nicht ersetzen.",
                             "The existing ffmpeg.exe could not be replaced.")));
    return;
  }
  if (!RenameOver(fresh, exe)) {
    if (replacing) RenameOver(old, exe);
    CleanUp(targetFolder);
    finish(false, CAP_SAID(T("Das neue ffmpeg.exe ließ sich nicht einsetzen.",
                             "The new ffmpeg.exe could not be put in place.")));
    return;
  }
  // Fails while the old one still runs; the next start or download takes it.
  CleanUp(targetFolder);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    resultPath_ = PathToUtf8(exe);
    installedVersion_ = version;
  }
  updateAvailable_.store(false, std::memory_order_relaxed);
  installed_.store(true, std::memory_order_relaxed);
  progress_.store(1.0f);
  CAP_LOG("ffmpeg downloaded: %s (version %s)", PathToUtf8(exe).c_str(), version.c_str());
  finish(true, CAP_SAID(version.empty()
                            ? std::string(T("ffmpeg ist bereit.", "ffmpeg is ready."))
                            : Format(T("ffmpeg %s ist bereit.", "ffmpeg %s is ready."),
                                     version.c_str())));
}

}  // namespace cap
