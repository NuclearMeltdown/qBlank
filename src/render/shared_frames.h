#pragma once

// The recording's pictures, handed over on the GPU.
//
// For a hardware encoder the yuv pass reads nothing back: it draws into one of
// three textures the recorder made on its own device and shared, and the
// recorder turns that into the encoder's NV12 or P010 on its side. Who may
// touch a texture says its state here. The order of the GPU work on the two
// devices says the keyed mutex every texture carries, by the key it was last
// released with: 1 once the yuv pass drew into it, 0 once the recorder took
// the picture out.

#include <atomic>
#include <cstdint>

namespace cap {

class SharedFrames {
 public:
  static const int kSlots = 3;
  enum State : uint8_t {
    Free,        // the yuv pass may take it
    Writing,     // the yuv pass draws into it
    Ready,       // the newest picture, for the recorder
    Converting,  // the recorder takes the picture out
  };

  virtual ~SharedFrames() = default;

  // The textures' NT handles, open as long as the object lives. Each is
  // `width` x `rows` of R8_UINT, or of R16_UINT with `sixteenBit`: the yuv
  // pass's image, the luma rows and the NV12 chroma rows after them.
  void* handle[kSlots] = {};
  int width = 0;
  int height = 0;  // luma rows
  int rows = 0;    // YuvRows of `height`
  bool sixteenBit = false;

  std::atomic<uint8_t> state[kSlots] = {};
  // Written by whoever holds the slot, read by the next one after the state
  // changed hands.
  uint64_t key[kSlots] = {};
};

}  // namespace cap
