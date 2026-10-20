#pragma once

// A recording without filters (RecordSettings::unfiltered) takes the card's
// frames as they arrive. Some go into the file as they are; the rest are
// repacked into what the encoder takes, once, on the writer's thread: the
// same samples in another order, none of them changed.

#include <cstddef>
#include <cstdint>

#include "capture/video_format.h"
#include "config.h"

namespace cap {

enum class CardRepack : uint8_t {
  None,       // pointed into as it is
  Rows,       // the same planes, the rows packed tight
  Planar422,  // packed 4:2:2 into planes of Y, U and V
  Full444,    // packed 4:2:2 into three full planes, each chroma sample twice
  Planar420,  // NV12 into planes of Y, U and V
  Gbr,        // B, G, R into planes of G, B and R, the top row first
  Bgr24,      // four bytes a pixel into three, the rows on four bytes
};

// What the encoder gets, laid out tight: the planes one after another.
enum class CardOut : uint8_t { Packed422, Nv12, P010, Yuv420p, Yuv422p, Yuv444p, Gbrp, Bgr24 };

struct CardPlan {
  const char* pixelFormat = nullptr;  // ffmpeg's name; null: the encoder takes none of it
  CardOut out = CardOut::Nv12;
  CardRepack repack = CardRepack::None;
  bool rgb = false;
  // What a pixel costs before the codec, for the disk estimate.
  double pixelBytes = 0.0;
};

// How `encoder` takes frames of `card`, rounded down to even sizes.
CardPlan PlanCard(RecordEncoder encoder, const VideoFormatInfo& card);

// Whether frames of `a` lie in memory as those of `b` do.
bool SameCardFormat(const VideoFormatInfo& a, const VideoFormatInfo& b);

// The bytes of the card's top plane row, and of one whole frame.
int CardStride(const VideoFormatInfo& card);
size_t CardFrameBytes(const VideoFormatInfo& card);

// What `out` takes for `width` x `height`, laid out tight.
size_t CardOutBytes(CardOut out, int width, int height);

// Points `data` and `linesize` at the planes of `width` x `height` the
// encoder takes: in `card` itself for CardRepack::None, else in `out`
// (CardOutBytes of it), written there from `card` first.
void RepackCard(const CardPlan& plan, const VideoFormatInfo& format, int width, int height,
                const uint8_t* card, uint8_t* out, uint8_t* data[4], int linesize[4]);

}  // namespace cap
