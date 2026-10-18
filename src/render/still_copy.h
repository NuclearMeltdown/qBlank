#pragma once

// A still on its way out of the GPU, for a screenshot.
//
// Reading a picture back used to wait for the GPU on the spot -- a millisecond
// or two, and a frame at two hundred and forty. Now the copy is only queued
// where the picture is drawn, and looked at again a frame or so later, when the
// GPU is long done with it. The bytes are then read out by the thread that
// saves them, straight from the mapped memory: copying eight megabytes on the
// thread that draws would cost another millisecond.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace cap {

// What a copy holds, four bytes or four half floats a pixel.
enum class StillLayout { Rgba8, Bgra8, Half4 };

// The backend's copy. Made, mapped and unmapped by the thread that draws, and
// destroyed before the device it came from. After Unmap it may be destroyed on
// any thread; before, only on the one that draws.
class StillCopy {
 public:
  virtual ~StillCopy() = default;

  enum class Map { Pending, Ready, Failed };
  // Never waits for the GPU: Pending while it is still copying. Ready hands
  // out the bytes, valid until Unmap or the copy is destroyed.
  virtual Map TryMap(const uint8_t** data, size_t* rowPitch) = 0;
  // The bytes are out. Whatever needs the thread that draws happens here.
  virtual void Unmap() {}

  int width = 0;
  int height = 0;
  StillLayout layout = StillLayout::Rgba8;
};

// The bytes of a mapped copy, read out once by whichever thread comes first:
// the one saving it, or the one that drew it when the copy has to go before
// that -- the graphics restart, the end of the program.
class StillPixels {
 public:
  StillPixels(const StillCopy& copy, const uint8_t* mapped, size_t rowPitch)
      : width(copy.width), height(copy.height), layout(copy.layout), mapped_(mapped),
        rowPitch_(rowPitch) {}

  StillPixels(const StillPixels&) = delete;
  StillPixels& operator=(const StillPixels&) = delete;

  // Out of the mapping, tightly packed: eight bit RGBA with the alpha opaque,
  // or the half floats as they are. Any thread; the second call finds it done.
  void Read();
  // Read has run, and the copy may go.
  bool read() const { return read_.load(std::memory_order_acquire); }

  // After Read, one of the two, by layout.
  const std::vector<uint8_t>& rgba() const { return rgba_; }
  const std::vector<uint16_t>& half() const { return half_; }

  const int width;
  const int height;
  const StillLayout layout;

 private:
  std::mutex mutex_;
  const uint8_t* const mapped_;
  const size_t rowPitch_;
  std::vector<uint8_t> rgba_;
  std::vector<uint16_t> half_;
  std::atomic<bool> read_{false};
};

}  // namespace cap
