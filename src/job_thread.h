#pragma once

// One thread for work that waits on something -- the disk, a device, the
// shell -- and must never hold up a frame. Jobs run one after the other, in the
// order they were posted. The thread starts with the first job and ends with
// the destructor, once its queue is empty.
//
// A job that has an answer for the thread that owns it returns what to do with
// it, and Collect() does that there. That way toasts, the config and the
// settings window are only ever touched by the thread that draws them.
//
// Declare it last among the members whose `this` its jobs capture: members go
// in reverse order, so it then finishes its jobs before any of them is gone.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cap {

class JobThread {
 public:
  // `onAnswer`, when given, runs on the thread each time an answer is ready:
  // for an owner that sleeps until something happens and has to be woken to
  // collect it.
  explicit JobThread(std::function<void()> onAnswer = nullptr) : onAnswer_(std::move(onAnswer)) {}
  ~JobThread();

  JobThread(const JobThread&) = delete;
  JobThread& operator=(const JobThread&) = delete;

  void Post(std::function<void()> job);
  // `job` runs on the thread; what it returns runs in the next Collect(). An
  // empty function is no answer.
  void Ask(std::function<std::function<void()>()> job);
  // Runs the answers that came back, on the caller's thread. Costs one atomic
  // load when there are none, so it can be called every frame.
  void Collect();
  // Waits until every job posted so far has run. For the end of a session,
  // when the process may be gone before the destructor would have waited.
  void Drain();

 private:
  void Loop();

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable posted_;
  std::condition_variable idle_;
  std::deque<std::function<void()>> jobs_;
  bool busy_ = false;  // a job is running
  bool quit_ = false;

  std::mutex answerMutex_;
  std::vector<std::function<void()>> answers_;
  std::atomic<bool> answered_{false};
  const std::function<void()> onAnswer_;
};

}  // namespace cap
