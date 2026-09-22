#pragma once

// Where captured frames wait for the render thread.
//
// A triple buffer: each incoming frame is copied into a free slot and the
// writer returns immediately, and the render thread always picks up the newest
// slot. Frames that arrive faster than we display are dropped rather than
// queued, so the picture is always as fresh as the card can make it.
//
// The capture backend writes, the render thread reads. How frames get here is
// the backend's business -- on Windows a DirectShow renderer filter, see
// frame_sink.h.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "capture/video_format.h"
#include "wake_signal.h"

namespace cap {

// Points at the slot the reader currently holds. Stays valid until the next
// AcquireFrame call on the same thread.
struct FrameView {
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint64_t sequence = 0;

  bool valid() const { return data != nullptr && size > 0; }
};

struct SinkStats {
  uint64_t received = 0;
  uint64_t dropped = 0;    // arrived but overwritten before being displayed
  uint64_t displayed = 0;  // picked up by the render thread
  double sourceFps = 0.0;  // measured arrival rate
  double lastArrivalAgeMs = 0.0;
};

class FrameBuffer {
 public:
  FrameBuffer() = default;

  FrameBuffer(const FrameBuffer&) = delete;
  FrameBuffer& operator=(const FrameBuffer&) = delete;

  // ---- writer side (the backend) ----

  // A connection with this format: sizes the slots for it and starts the
  // counters over.
  void Configure(const VideoFormatInfo& info);

  // The connection is gone.
  void Clear();

  // A format that came along with a frame. `pixelsChanged` is the backend's
  // verdict on the pixel format -- it knows it by a finer name than the label.
  // A different size or pixel format empties the slots; either way the format
  // is taken.
  void Reformat(const VideoFormatInfo& info, bool pixelsChanged);

  // Forgets the frame waiting to be picked up (flush, stop).
  void DropPending();

  // Copies one frame in and publishes it. One writer at a time.
  void Write(const uint8_t* data, size_t length);

  // ---- reader side (render thread) ----

  // Moves the newest completed frame into the reader's hands. Returns true when
  // that frame is one the reader has not seen yet. `out` always describes the
  // currently held frame, even when it is the previous one.
  bool AcquireFrame(FrameView* out);

  VideoFormatInfo format() const;
  SinkStats stats() const;

  // When the newest frame was handed over by the driver, in ClockTicks. Zero
  // before the first one. Scheduling anything from the moment we get round to
  // drawing instead of from this is how a second field ends up due after the
  // next frame has already arrived.
  int64_t lastArrivalTicks() const;
  void ResetStats();

  // True when a frame arrived within the given window -- used to show the
  // "no signal" state without tearing the capture down.
  bool HasRecentFrame(double withinSeconds) const;

  // Signalled whenever a frame is published, so the render loop can sleep
  // instead of polling.
  WakeSignal& frameReady() { return frameReady_; }

 private:
  int PickWriteSlotLocked() const;

  WakeSignal frameReady_;

  mutable std::mutex mutex_;
  std::vector<uint8_t> slots_[3];
  size_t slotSize_[3] = {0, 0, 0};
  int readyIdx_ = -1;  // newest completed frame, not yet taken by the reader
  int readIdx_ = -1;   // slot the reader currently holds
  uint64_t sequence_ = 0;
  uint64_t readSequence_ = 0;
  VideoFormatInfo format_;

  // Stats.
  uint64_t received_ = 0;
  uint64_t dropped_ = 0;
  uint64_t displayed_ = 0;
  int64_t lastArrivalTicks_ = 0;
  double measuredFps_ = 0.0;
  int64_t fpsWindowStartTicks_ = 0;
  uint64_t fpsWindowFrames_ = 0;
};

}  // namespace cap
