#pragma once

// Fetches a static ffmpeg build on request and puts ffmpeg.exe next to qBlank.
//
// Downloaded from the upstream, never shipped alongside us -- and the licence is
// not why. Whether a second executable lands on the machine is the user's
// decision; without it qBlank is a single file of about 2 MB; and what is
// fetched here is whatever the current release build is, where a copy in the
// repository would sit at the version somebody last committed. Those reasons
// hold regardless of what either program is licensed under.
//
// The licence only agrees. The usual Windows builds carry x264 and x265 and are
// therefore GPL, and whoever hands them on owes the source with them; fetching
// from gyan.dev on a button press is not distribution, and calling ffmpeg as a
// child process is not linking, so the two stay separate programs.

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "common.h"

namespace cap {

class FfmpegDownloader {
 public:
  enum class State { Idle, Running, Done, Failed };

  FfmpegDownloader() = default;
  ~FfmpegDownloader();

  FfmpegDownloader(const FfmpegDownloader&) = delete;
  FfmpegDownloader& operator=(const FfmpegDownloader&) = delete;

  // Downloads, verifies, extracts, cleans up. Returns false when one is already
  // running. `targetFolder` receives ffmpeg.exe directly.
  bool Start(const std::filesystem::path& targetFolder);

  // Asks the server which version it would deliver, without downloading. Runs on
  // the same worker, so it is also asynchronous.
  bool StartVersionCheck();

  void Cancel();

  State state() const { return state_.load(std::memory_order_relaxed); }
  bool busy() const { return state() == State::Running; }

  // 0..1 while downloading, negative when the size is unknown.
  float progress() const { return progress_.load(std::memory_order_relaxed); }

  // What is happening right now, or what went wrong.
  std::string message() const;
  // Filled in after a successful download or version check.
  std::string remoteVersion() const;
  std::string resultPath() const;

 private:
  void Run(std::filesystem::path targetFolder, bool versionOnly);
  void SetMessage(const std::string& text);

  std::thread thread_;
  std::atomic<State> state_{State::Idle};
  std::atomic<float> progress_{-1.0f};
  std::atomic<bool> cancel_{false};

  mutable std::mutex mutex_;
  std::string message_;
  std::string remoteVersion_;
  std::string resultPath_;
};

}  // namespace cap
