#pragma once

// The capture driver's own settings dialog. None of it is ours: the pages come
// out of the driver and differ from card to card. All we do is find them and
// put a frame around them.

#include <dshow.h>

#include <atomic>
#include <string>
#include <thread>

#include "common_win32.h"

namespace cap {

class DevicePropertyPages {
 public:
  ~DevicePropertyPages();

  DevicePropertyPages(const DevicePropertyPages&) = delete;
  DevicePropertyPages& operator=(const DevicePropertyPages&) = delete;
  DevicePropertyPages() = default;

  // True when the filter offers property pages at all. Plenty of cards do not,
  // and a button that opens an empty dialog is worse than one that says so.
  static bool Available(IBaseFilter* filter);

  // Shows the pages on a thread of their own, so the render loop keeps going and
  // the picture updates while a slider is being dragged. Returns false when
  // there is nothing to show or one is already open; `error` then says which.
  bool Open(IBaseFilter* filter, const std::wstring& title, std::string* error);

  bool busy() const { return running_.load(std::memory_order_relaxed); }

 private:
  void Run(std::wstring title);

  // Held for as long as the dialog is up, so the graph can be torn down
  // underneath it without the pages losing their object. Assigned and released
  // on the calling thread.
  ComPtr<IBaseFilter> filter_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace cap
