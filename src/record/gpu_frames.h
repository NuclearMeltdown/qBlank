#pragma once

// The recorder's half of the pictures handed over on the GPU (shared_frames.h).
//
// A hardware encoder takes its frames from a pool on a device of its own. This
// is that device, on the adapter the renderer draws with, with the pool, the
// three textures the yuv pass draws into, and the step between them: one small
// pass that splits a texture's luma and chroma rows into a frame of the pool,
// NV12 or P010. The picture never leaves the GPU.

#include <atomic>
#include <cstdint>
#include <memory>

struct AVBufferRef;
struct AVFrame;

namespace cap {

struct Libav;
class SharedFrames;

class GpuFrames {
 public:
  virtual ~GpuFrames() = default;

  // The textures, for the renderer.
  virtual std::shared_ptr<SharedFrames> frames() const = 0;
  // The pool, for the encoder's hw_frames_ctx and av_hwframe_get_buffer.
  virtual AVBufferRef* framesContext() const = 0;

  // Takes the picture out of `slot`, which the caller holds as Converting, into
  // `out`, a frame of the pool. Waits for the yuv pass to finish drawing it for
  // as long as `keepWaiting` holds, a second at most. False when it gave up;
  // either way the slot can go back to Free afterwards.
  virtual bool Convert(int slot, AVFrame* out, const std::atomic<bool>& keepWaiting) = 0;
};

// Null when any of it cannot be made on the adapter with this LUID, which the
// log then says; the recording reads the picture back instead. `width` and
// `height` are even; ten bits on sixteen with `sixteenBit`.
std::unique_ptr<GpuFrames> CreateGpuFrames(const Libav& av, uint64_t adapter, int width,
                                           int height, bool sixteenBit);

}  // namespace cap
