#include "ui/file_dialog.h"

namespace cap {

AsyncFileDialog::~AsyncFileDialog() {
  // The dialog is modal to the user, not to us: there is no way to cancel it
  // from here, so the only correct thing on shutdown is to wait for it.
  if (thread_.joinable()) thread_.join();
}

bool AsyncFileDialog::Start(const FileDialogRequest& request, const Window* owner, int tag) {
  if (running_.load(std::memory_order_relaxed)) return false;
  if (thread_.joinable()) thread_.join();  // reap the previous one

  {
    std::lock_guard<std::mutex> lock(mutex_);
    results_.clear();
  }
  tag_ = tag;
  ready_.store(false, std::memory_order_relaxed);
  running_.store(true, std::memory_order_relaxed);
  thread_ = std::thread([this, request, owner] { Deliver(ShowFileDialog(request, owner)); });
  return true;
}

bool AsyncFileDialog::TakeResult(std::vector<std::filesystem::path>* out, int* tag) {
  if (!ready_.load(std::memory_order_acquire)) return false;
  ready_.store(false, std::memory_order_relaxed);
  if (thread_.joinable()) thread_.join();
  if (out) {
    std::lock_guard<std::mutex> lock(mutex_);
    *out = results_;
  }
  if (tag) *tag = tag_;
  return true;
}

void AsyncFileDialog::Deliver(std::vector<std::filesystem::path> picked) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    results_ = std::move(picked);
  }
  running_.store(false, std::memory_order_relaxed);
  ready_.store(true, std::memory_order_release);
}

}  // namespace cap
