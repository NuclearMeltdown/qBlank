#pragma once

// The Windows half of wake_signal.h, for code that waits on a WakeSignal
// together with other objects -- the message loop does.

#include <windows.h>

#include "wake_signal.h"

namespace cap {

struct WakeSignal::Native {
  HANDLE event = nullptr;  // auto-reset
};

// The event behind the signal, or null when it could not be created.
inline HANDLE NativeHandle(const WakeSignal& signal) {
  return signal.native() ? signal.native()->event : nullptr;
}

}  // namespace cap
