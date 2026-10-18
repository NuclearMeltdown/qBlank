#include "job_thread.h"

#include "platform.h"

namespace cap {

JobThread::~JobThread() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  posted_.notify_one();
  if (thread_.joinable()) thread_.join();
}

void JobThread::Post(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(std::move(job));
    if (!thread_.joinable()) thread_ = std::thread(&JobThread::Loop, this);
  }
  posted_.notify_one();
}

void JobThread::Ask(std::function<std::function<void()>()> job) {
  Post([this, job = std::move(job)] {
    std::function<void()> answer = job();
    if (!answer) return;
    {
      std::lock_guard<std::mutex> lock(answerMutex_);
      answers_.push_back(std::move(answer));
      answered_.store(true, std::memory_order_release);
    }
    if (onAnswer_) onAnswer_();
  });
}

void JobThread::Collect() {
  if (!answered_.load(std::memory_order_acquire)) return;
  std::vector<std::function<void()>> answers;
  {
    std::lock_guard<std::mutex> lock(answerMutex_);
    answers.swap(answers_);
    answered_.store(false, std::memory_order_relaxed);
  }
  for (std::function<void()>& answer : answers) answer();
}

void JobThread::Drain() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
}

void JobThread::Loop() {
  SystemThreadScope onThisThread;
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    posted_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
    if (jobs_.empty()) return;  // told to quit, and nothing left to do
    std::function<void()> job = std::move(jobs_.front());
    jobs_.pop_front();
    busy_ = true;
    lock.unlock();
    job();
    job = nullptr;
    lock.lock();
    busy_ = false;
    if (jobs_.empty()) idle_.notify_all();
  }
}

}  // namespace cap
