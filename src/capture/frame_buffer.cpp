#include "capture/frame_buffer.h"

#include <atomic>
#include <cstring>

#include "common.h"

namespace cap {

namespace {

uint32_t NextFormatGen() {
  static std::atomic<uint32_t> next{0};
  return ++next;
}

}  // namespace

// ------------------------------------------------------------- writer side

void FrameBuffer::Configure(const VideoFormatInfo& info) {
  std::lock_guard<std::mutex> lock(mutex_);
  format_ = info;
  formatGen_ = NextFormatGen();
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
    // Die Slots bleiben stehen. Einen davon haelt der Renderthread womoeglich
    // gerade und laedt aus ihm hoch; ihn neu anzulegen gab bei einem groesseren
    // Bild seinen Speicher frei, waehrend daraus gelesen wurde. Write
    // vergroessert einen Slot ohnehin erst, wenn es hineinschreibt, und nimmt
    // nie den des Lesers. Nur das wartende Bild ist im alten Format und geht.
    readyIdx_ = -1;
  }
  format_ = info;
  formatGen_ = NextFormatGen();
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
    slotGen_[slot] = formatGen_;
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
      out->formatGen = slotGen_[readIdx_];
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

bool FrameBuffer::FormatOf(uint32_t gen, VideoFormatInfo* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (gen != formatGen_) return false;
  *out = format_;
  return true;
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
