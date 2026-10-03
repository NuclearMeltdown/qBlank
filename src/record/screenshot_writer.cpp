#include "record/screenshot_writer.h"

#include "platform.h"
#include "record/screenshot.h"

namespace cap {
namespace {

// Shots waiting at most. Holding the key down should not fill the memory with
// pictures faster than the disk takes them.
const size_t kMaxWaiting = 4;

}  // namespace

ScreenshotWriter::~ScreenshotWriter() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_one();
  if (thread_.joinable()) thread_.join();
}

bool ScreenshotWriter::Queue(Job&& job) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (jobs_.size() >= kMaxWaiting) return false;
    jobs_.push_back(std::move(job));
  }
  // Started with the first shot: most sessions never take one.
  if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
  wake_.notify_one();
  return true;
}

bool ScreenshotWriter::Take(Result* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (done_.empty()) return false;
  *out = std::move(done_.front());
  done_.pop_front();
  return true;
}

void ScreenshotWriter::Run() {
  SystemThreadScope onThisThread;  // WIC
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (jobs_.empty()) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }

    Result result;
    result.note = std::move(job.note);
    result.width = job.width;
    result.height = job.height;
    result.path = job.wide ? MakeHdrScreenshotPath(job.folder, job.hdrFormat)
                           : MakeScreenshotPath(job.folder, job.format);
    if (result.path.empty()) {
      result.noFolder = true;
      result.path = std::move(job.folder);
    } else if (!job.wide) {
      result.ok = SaveScreenshot(result.path, job.pixels.data(), job.width, job.height,
                                 job.format, job.jpegQuality, &result.error);
    } else if (job.hdrFormat == HdrShotFormat::Avif) {
      result.ok = SaveScreenshotAvif(result.path, job.ffmpeg, job.halfPixels.data(), job.width,
                                     job.height, job.halfStride, job.paperWhiteNits,
                                     &result.error);
    } else {
      result.ok = SaveScreenshotHdr(result.path, job.halfPixels.data(), job.width, job.height,
                                    job.halfStride, job.paperWhiteNits, &result.error);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    done_.push_back(std::move(result));
  }
}

}  // namespace cap
