#pragma once

// Saves screenshots on a thread of its own.
//
// The picture is grabbed where it is drawn, that has to be; the rest does not.
// A PNG of a large picture takes longer to encode than a frame lasts, and AVIF
// goes through a whole ffmpeg run -- on the render thread, the picture stood
// still for that long. Shots are saved in the order they were taken, which is
// also what keeps two in the same second from picking the same name.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"

namespace cap {

class ScreenshotWriter {
 public:
  struct Job {
    std::filesystem::path folder;
    // Eight bit pixels for an ordinary shot, half floats for one that keeps
    // the range.
    bool wide = false;
    std::vector<uint8_t> pixels;
    std::vector<uint16_t> halfPixels;
    int width = 0;
    int height = 0;
    int halfStride = 0;
    ScreenshotFormat format = ScreenshotFormat::Png;
    int jpegQuality = 92;
    HdrShotFormat hdrFormat = HdrShotFormat::Jxr;
    std::filesystem::path ffmpeg;
    float paperWhiteNits = 0.0f;
    // Passed through to the result, for the message.
    std::string note;
  };

  struct Result {
    bool ok = false;
    // The folder could not be made; `path` is the folder then.
    bool noFolder = false;
    std::filesystem::path path;
    std::string error;
    std::string note;
    int width = 0;
    int height = 0;
  };

  ScreenshotWriter() = default;
  // Saves what was handed over before it goes.
  ~ScreenshotWriter();

  ScreenshotWriter(const ScreenshotWriter&) = delete;
  ScreenshotWriter& operator=(const ScreenshotWriter&) = delete;

  // False while too many are still waiting: each holds a whole picture.
  bool Queue(Job&& job);
  // One finished shot, oldest first. For the thread that queues.
  bool Take(Result* out);

 private:
  void Run();

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> jobs_;
  std::deque<Result> done_;
  std::thread thread_;
  bool stop_ = false;
};

}  // namespace cap
