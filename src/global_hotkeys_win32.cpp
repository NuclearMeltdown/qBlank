#include "global_hotkeys.h"

#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <thread>

#include "keys_win32.h"

namespace cap {
namespace {

constexpr int kCount = (int)HotkeyAction::Count;

// Posted by the main thread: the wanted set has changed.
constexpr UINT kMsgApply = WM_APP + 1;

// A hotkey's id is its action plus one; zero is not a valid id.
int IdOf(int action) {
  return action + 1;
}

}  // namespace

struct SystemHotkeys::Impl {
  std::thread thread;
  DWORD threadId = 0;
  DWORD wakeThread = 0;  // the thread Hold() was called from

  // The main thread's own: what it last handed over.
  HotkeyBinding asked[kCount];

  std::mutex mutex;
  std::condition_variable started;
  bool startDone = false;
  HotkeyBinding wanted[kCount];
  bool refused[kCount] = {};
  std::vector<HotkeyAction> pressed;

  // The hotkey thread's own.
  HotkeyBinding held[kCount];
  bool registered[kCount] = {};

  void Run();
  void Apply();
  void Stop();
  void Wake() { ::PostThreadMessageW(wakeThread, WM_NULL, 0, 0); }
};

void SystemHotkeys::Impl::Run() {
  ::SetThreadDescription(::GetCurrentThread(), L"qBlank hotkeys");
  // The queue, before anyone posts to it: the thread has none until it asks.
  MSG msg = {};
  ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  {
    std::lock_guard<std::mutex> lock(mutex);
    startDone = true;
  }
  started.notify_all();

  Apply();
  // No window: RegisterHotKey without one posts WM_HOTKEY to this thread.
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_HOTKEY) {
      const int action = (int)msg.wParam - 1;
      if (action < 0 || action >= kCount) continue;
      {
        std::lock_guard<std::mutex> lock(mutex);
        pressed.push_back((HotkeyAction)action);
      }
      Wake();
    } else if (msg.message == kMsgApply) {
      Apply();
    }
  }
  for (int i = 0; i < kCount; ++i) {
    if (registered[i]) ::UnregisterHotKey(nullptr, IdOf(i));
    registered[i] = false;
    held[i] = HotkeyBinding{};
  }
}

void SystemHotkeys::Impl::Apply() {
  HotkeyBinding want[kCount];
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (int i = 0; i < kCount; ++i) want[i] = wanted[i];
  }
  // All the letting go first: two actions swapping keys would otherwise find
  // each other's key still taken -- by this very thread.
  for (int i = 0; i < kCount; ++i) {
    if (held[i] == want[i]) continue;
    if (registered[i]) ::UnregisterHotKey(nullptr, IdOf(i));
    registered[i] = false;
  }
  for (int i = 0; i < kCount; ++i) {
    if (held[i] == want[i]) continue;
    held[i] = want[i];
    if (!want[i].bound()) continue;
    const UINT mods = MOD_NOREPEAT | (want[i].ctrl ? MOD_CONTROL : 0) |
                      (want[i].shift ? MOD_SHIFT : 0) | (want[i].alt ? MOD_ALT : 0);
    registered[i] = ::RegisterHotKey(nullptr, IdOf(i), mods, VirtualKeyOf(want[i].key)) != 0;
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (int i = 0; i < kCount; ++i) refused[i] = held[i].bound() && !registered[i];
  }
  Wake();  // a refusal shows without waiting for the next picture
}

void SystemHotkeys::Impl::Stop() {
  if (!thread.joinable()) return;
  ::PostThreadMessageW(threadId, WM_QUIT, 0, 0);
  thread.join();
  std::lock_guard<std::mutex> lock(mutex);
  for (bool& r : refused) r = false;
  pressed.clear();
}

SystemHotkeys::SystemHotkeys() : impl_(std::make_unique<Impl>()) {}

SystemHotkeys::~SystemHotkeys() {
  impl_->Stop();
}

void SystemHotkeys::Hold(const HotkeyBinding (&bindings)[kCount]) {
  Impl& h = *impl_;
  bool same = true;
  bool any = false;
  for (int i = 0; i < kCount; ++i) {
    if (!(bindings[i] == h.asked[i])) same = false;
    if (bindings[i].bound()) any = true;
  }
  if (same) return;
  for (int i = 0; i < kCount; ++i) h.asked[i] = bindings[i];

  if (!any) {
    h.Stop();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(h.mutex);
    for (int i = 0; i < kCount; ++i) h.wanted[i] = bindings[i];
  }
  if (h.thread.joinable()) {
    ::PostThreadMessageW(h.threadId, kMsgApply, 0, 0);
    return;
  }
  h.wakeThread = ::GetCurrentThreadId();
  {
    std::lock_guard<std::mutex> lock(h.mutex);
    h.startDone = false;
  }
  h.thread = std::thread([&h] { h.Run(); });
  h.threadId = ::GetThreadId(h.thread.native_handle());
  std::unique_lock<std::mutex> lock(h.mutex);
  h.started.wait(lock, [&h] { return h.startDone; });
}

bool SystemHotkeys::Refused(HotkeyAction action) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->refused[(int)action];
}

std::vector<HotkeyAction> SystemHotkeys::TakePressed() {
  std::vector<HotkeyAction> out;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  out.swap(impl_->pressed);
  return out;
}

}  // namespace cap
