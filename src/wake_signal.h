#pragma once

// Wakes a thread that sleeps until something happens -- a frame arrived, say.
//
// Auto-reset: one successful wait takes the signal away, and signals given while
// nobody waits collapse into one. That is the right shape for "there is news",
// where the waiter goes and looks for the newest state anyway.

#include <memory>

namespace cap {

class WakeSignal {
 public:
  WakeSignal();
  ~WakeSignal();

  WakeSignal(const WakeSignal&) = delete;
  WakeSignal& operator=(const WakeSignal&) = delete;

  void Signal();

  // Waits up to `timeoutMs` for the signal and takes it. Zero only looks. True
  // when it was signalled.
  bool Wait(int timeoutMs);

  // The platform's own object behind it, for code that waits on several things
  // at once. Defined by the platform half.
  struct Native;
  const Native* native() const { return native_.get(); }

 private:
  std::unique_ptr<Native> native_;
};

}  // namespace cap
