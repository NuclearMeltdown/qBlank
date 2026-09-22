#pragma once

// Reads a DirectShow audio input into an AudioRing.
//
// Needed because some capture cards never publish their embedded audio as a
// Windows sound device -- WASAPI cannot see it at all, but DirectShow can. The
// graph is the same shape as the video one: source filter straight into a sink
// of ours that converts and hands the samples over, with no reference clock so
// nothing is scheduled or held back.

#include <dshow.h>

#include <atomic>
#include <functional>
#include <string>

#include "audio/pcm.h"
#include "common_win32.h"
#include "config.h"

namespace cap {

// Called from the graph's streaming thread with interleaved stereo float.
// Stored once, so the indirection costs nothing per sample.
using AudioSinkFn = std::function<void(const float* interleaved, size_t frames)>;

class DShowAudioCapture {
 public:
  DShowAudioCapture() = default;
  ~DShowAudioCapture();

  DShowAudioCapture(const DShowAudioCapture&) = delete;
  DShowAudioCapture& operator=(const DShowAudioCapture&) = delete;

  // `sink` is called for every packet the graph delivers, as interleaved stereo
  // float at the rate reported by format(). It must stay valid until Stop.
  bool Start(const DeviceRef& device, AudioSinkFn sink, std::string* error);
  void Stop();

  bool running() const { return control_ != nullptr; }
  StreamFormat format() const { return format_; }

 private:
  void Teardown();

  ComPtr<IGraphBuilder> graph_;
  ComPtr<IMediaControl> control_;
  ComPtr<IBaseFilter> sourceFilter_;
  ComPtr<IBaseFilter> sinkFilter_;
  StreamFormat format_;
};

}  // namespace cap
