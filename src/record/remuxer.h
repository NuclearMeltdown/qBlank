#pragma once

// Rewraps a recording into a different container without touching the picture.
//
// Recording defaults to MKV because it survives a crash: an MP4 whose header was
// never finalised has no moov atom and will not open at all, while a truncated
// MKV plays right up to the point the power went out. MP4 is what everything
// else wants to be handed, so the two together need this step in between.
//
// Nothing is re-encoded. The frames are copied across byte for byte, which takes
// seconds rather than the length of the recording and cannot lose quality.

#include <atomic>
#include <mutex>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "common.h"

namespace cap {

class Remuxer {
 public:
  enum class State { Idle, Running, Done, Failed };

  struct Item {
    std::filesystem::path input;
    std::filesystem::path output;  // filled in once known
    bool done = false;
    bool ok = false;
    std::string error;
  };

  Remuxer() = default;
  ~Remuxer();

  Remuxer(const Remuxer&) = delete;
  Remuxer& operator=(const Remuxer&) = delete;

  // Rewraps every file into `.mp4` beside itself. Returns false if one is
  // already running or the list is empty. Existing files are never overwritten;
  // a numbered suffix is used instead.
  bool Start(const std::filesystem::path& ffmpegPath,
             const std::vector<std::filesystem::path>& inputs);

  void Cancel();

  State state() const { return state_.load(std::memory_order_relaxed); }
  bool busy() const { return state() == State::Running; }

  // 0..1 across the whole batch.
  float progress() const { return progress_.load(std::memory_order_relaxed); }
  std::string message() const;
  std::vector<Item> items() const;

  // Number of finished files and how many of those worked.
  int doneCount() const { return done_.load(std::memory_order_relaxed); }
  int okCount() const { return ok_.load(std::memory_order_relaxed); }

 private:
  void Run(std::filesystem::path ffmpegPath);
  void SetMessage(const std::string& text);

  std::thread thread_;
  std::atomic<State> state_{State::Idle};
  std::atomic<float> progress_{0.0f};
  std::atomic<bool> cancel_{false};
  std::atomic<int> done_{0};
  std::atomic<int> ok_{0};

  mutable std::mutex mutex_;
  std::string message_;
  std::vector<Item> items_;
};

}  // namespace cap
