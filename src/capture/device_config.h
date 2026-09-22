#pragma once

// The capture driver's own settings dialog. None of it is ours: the pages come
// out of the driver and differ from card to card. All we do is find them and
// put a frame around them.

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace cap {

class VideoCapture;

class DevicePropertyPages {
 public:
  ~DevicePropertyPages();

  DevicePropertyPages(const DevicePropertyPages&) = delete;
  DevicePropertyPages& operator=(const DevicePropertyPages&) = delete;
  DevicePropertyPages() = default;

  // Shows the pages of the running card on a thread of their own, so the render
  // loop keeps going and the picture updates while a slider is being dragged.
  // Returns false when there is nothing to show or one is already open; `error`
  // then says which. Plenty of cards bring no pages at all, and a button that
  // opens an empty dialog is worse than one that says so.
  bool Open(const VideoCapture& capture, const std::string& title, std::string* error);

  bool busy() const { return running_.load(std::memory_order_relaxed); }

 private:
  void Run();

  // Held for as long as the dialog is up, so the device can be torn down
  // underneath it without the pages losing their object. Assigned and released
  // on the calling thread.
  std::function<void()> show_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace cap
