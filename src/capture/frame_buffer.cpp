#include "capture/frame_buffer.h"

#include <cstring>

#include "common.h"

namespace cap {

// ------------------------------------------------------------- writer side

void FrameBuffer::Configure(const VideoFormatInfo& info) {
  std::lock_guard<std::mutex> lock(mutex_);
  format_ = info;
  for (int i = 0; i < 3; ++i) {
    slots_[i].assign(info.imageSize, 0);
    slotSize_[i] = 0;
  }
  readyIdx_ = -1;
  readIdx_ = -1;
  sequence_ = 0;
  readSequence_ = 0;
  received_ = dropped_ = displayed_ = 0;
  lastArrivalTicks_ = 0;
  measuredFps_ = 0.0;
  fpsWindowStartTicks_ = 0;
  fpsWindowFrames_ = 0;
}

void FrameBuffer::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  format_ = VideoFormatInfo{};
  readyIdx_ = -1;
  readIdx_ = -1;
}

void FrameBuffer::Reformat(const VideoFormatInfo& info, bool pixelsChanged) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (info.width != format_.width || info.height != format_.height || pixelsChanged) {
    CAP_LOG("Format change mid-stream: %s %dx%d", info.subtypeLabel.c_str(), info.width,
            info.height);
    for (int i = 0; i < 3; ++i) {
      slots_[i].assign(info.imageSize, 0);
      slotSize_[i] = 0;
    }
    readyIdx_ = -1;
    readIdx_ = -1;
  }
  format_ = info;
}

void FrameBuffer::DropPending() {
  std::lock_guard<std::mutex> lock(mutex_);
  readyIdx_ = -1;
}

int FrameBuffer::PickWriteSlotLocked() const {
  for (int i = 0; i < 3; ++i) {
    if (i != readyIdx_ && i != readIdx_) return i;
  }
  return 0;  // unreachable with three slots and two reserved indices
}

void FrameBuffer::Write(const uint8_t* data, size_t length) {
  int slot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    slot = PickWriteSlotLocked();
    if (length > slots_[slot].size()) slots_[slot].resize(length);
  }

  // Copy outside the lock. The chosen slot is neither the one the reader holds
  // nor the one waiting to be picked up, so nobody else can touch it. There is
  // only one writer.
  memcpy(slots_[slot].data(), data, length);

  const int64_t now = ClockTicks();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (readyIdx_ >= 0) ++dropped_;  // previous frame never made it to the screen
    slotSize_[slot] = length;
    readyIdx_ = slot;
    ++sequence_;
    ++received_;
    lastArrivalTicks_ = now;

    // Arrival rate over a rolling one second window.
    if (fpsWindowStartTicks_ == 0) {
      fpsWindowStartTicks_ = now;
      fpsWindowFrames_ = 0;
    }
    ++fpsWindowFrames_;
    const double elapsed = TicksToSeconds(now - fpsWindowStartTicks_);
    if (elapsed >= 1.0) {
      measuredFps_ = (double)fpsWindowFrames_ / elapsed;
      fpsWindowStartTicks_ = now;
      fpsWindowFrames_ = 0;
    }
  }
  frameReady_.Signal();
}

// ------------------------------------------------------------- reader side

int64_t FrameBuffer::lastArrivalTicks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lastArrivalTicks_;
}

bool FrameBuffer::AcquireFrame(FrameView* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool isNew = false;
  if (readyIdx_ >= 0) {
    readIdx_ = readyIdx_;
    readyIdx_ = -1;
    readSequence_ = sequence_;
    ++displayed_;
    isNew = true;
  }
  if (out) {
    if (readIdx_ >= 0) {
      out->data = slots_[readIdx_].data();
      out->size = slotSize_[readIdx_];
      out->sequence = readSequence_;
    } else {
      *out = FrameView{};
    }
  }
  return isNew;
}

VideoFormatInfo FrameBuffer::format() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return format_;
}

SinkStats FrameBuffer::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  SinkStats s;
  s.received = received_;
  s.dropped = dropped_;
  s.displayed = displayed_;
  s.sourceFps = measuredFps_;
  s.lastArrivalAgeMs =
      lastArrivalTicks_ ? TicksToSeconds(ClockTicks() - lastArrivalTicks_) * 1000.0 : -1.0;
  return s;
}

void FrameBuffer::ResetStats() {
  std::lock_guard<std::mutex> lock(mutex_);
  received_ = dropped_ = displayed_ = 0;
  measuredFps_ = 0.0;
  fpsWindowStartTicks_ = 0;
  fpsWindowFrames_ = 0;
}

bool FrameBuffer::HasRecentFrame(double withinSeconds) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (lastArrivalTicks_ == 0) return false;
  return TicksToSeconds(ClockTicks() - lastArrivalTicks_) <= withinSeconds;
}

}  // namespace cap
