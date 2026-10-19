#pragma once

// Fetches the ffmpeg libraries on request and puts them next to qBlank.
//
// Downloaded from upstream, never shipped alongside us. Whether a hundred
// megabytes of codecs land on the machine is the user's decision; without them
// qBlank is a single file of about 2 MB; and what is fetched is the newest
// release of the major the program was built against, where a copy in the
// repository would sit at whatever somebody last committed.
//
// The source is Gyan's shared build, from GyanD/codexffmpeg on GitHub: the
// same builds gyan.dev links, as releases with a SHA-256 per file. Of the
// archive only the four libraries qBlank opens are kept (libav.h). The build
// carries x264 and x265 and is GPL, and qBlank is GPL too, so loading it into
// the process is fine.
//
// An update never writes over libraries in use. It is unpacked next to them and
// moved in at the next start, or at once when nothing has loaded them yet.

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "common.h"
#include "record/ffmpeg_locator.h"

namespace cap {

class FfmpegDownloader {
 public:
  enum class State { Idle, Running, Done, Failed };

  FfmpegDownloader() = default;
  ~FfmpegDownloader();

  FfmpegDownloader(const FfmpegDownloader&) = delete;
  FfmpegDownloader& operator=(const FfmpegDownloader&) = delete;

  // Downloads, verifies, unpacks. Returns false when one is already running.
  bool Start(const std::filesystem::path& targetFolder);

  // Asks which version a download would deliver, without downloading. Runs on
  // the same worker, so it is also asynchronous. `installed` is the release
  // number of the libraries in use, when there are any to compare with;
  // `announce` marks the answer as worth a popup, which is what the check at
  // startup wants.
  bool StartVersionCheck(const std::string& installed = {}, bool announce = false);

  void Cancel();

  // Moves a finished download into place and clears what is left over: the
  // ffmpeg.exe earlier versions ran, the libraries an update replaced, a
  // download that never finished. At startup, before anything loads them.
  static void InstallPending(const std::filesystem::path& targetFolder);

  State state() const { return state_.load(std::memory_order_relaxed); }
  bool busy() const { return state() == State::Running; }

  // 0..1 while downloading, negative when the size is unknown.
  float progress() const { return progress_.load(std::memory_order_relaxed); }

  // What is happening right now, or what went wrong.
  std::string message() const;
  // Filled in after a successful download or version check.
  std::string remoteVersion() const;
  // The installed number the last version check compared against.
  std::string installedVersion() const;

  // The last version check found a newer release than the installed one. Stays
  // set through a failed download, so the offer does not vanish with it.
  bool updateAvailable() const { return updateAvailable_.load(std::memory_order_relaxed); }
  // The last version check was the one made at startup.
  bool announced() const { return announce_.load(std::memory_order_relaxed); }
  // A download is waiting for the next start, because the libraries it
  // replaces are in use.
  bool pendingRestart() const { return pending_.load(std::memory_order_relaxed); }

  // True once after a download has put the libraries in place, for whoever
  // loads them to try again.
  bool TakeInstalled() { return installed_.exchange(false, std::memory_order_relaxed); }

 private:
  void Run(std::filesystem::path targetFolder, bool versionOnly);
  void SetMessage(const std::string& text);

  std::thread thread_;
  std::atomic<State> state_{State::Idle};
  std::atomic<float> progress_{-1.0f};
  std::atomic<bool> cancel_{false};
  std::atomic<bool> updateAvailable_{false};
  std::atomic<bool> announce_{false};
  std::atomic<bool> installed_{false};
  std::atomic<bool> pending_{false};

  mutable std::mutex mutex_;
  std::string message_;
  std::string remoteVersion_;
  std::string installedVersion_;
};

}  // namespace cap
