#include "wake_signal_win32.h"

#include <new>

namespace cap {

WakeSignal::WakeSignal() : native_(new (std::nothrow) Native) {
  if (native_) native_->event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

WakeSignal::~WakeSignal() {
  if (native_ && native_->event) ::CloseHandle(native_->event);
}

void WakeSignal::Signal() {
  if (HANDLE ev = NativeHandle(*this)) ::SetEvent(ev);
}

bool WakeSignal::Wait(int timeoutMs) {
  HANDLE ev = NativeHandle(*this);
  return ev && ::WaitForSingleObject(ev, (DWORD)timeoutMs) == WAIT_OBJECT_0;
}

}  // namespace cap
