#pragma once

// The two ends of an audio stream as the platform provides them: an input that
// delivers what a device records, an output that asks for what it should play.
// Only the device work lives behind these -- opening it, its format, the
// wakeups, converting to and from its own sample layout. The ring, the target
// fill and the drift correction are AudioEngine's, written once.
//
// Checked on paper against WASAPI and PipeWire, so that it is not simply the
// first one with the names changed:
//   - an input is an event-driven capture client on one, a pw_stream in the
//     input direction on the other. Both hand over packets in the device's own
//     format on a thread of their own, and both learn that format only once the
//     stream is open (GetMixFormat, the format param arriving);
//   - an output is a render client asking for the free part of its buffer on
//     one, a pw_stream's process callback asking for a buffer on the other.
//     Both pull, which is why AudioOutput asks the engine what to play instead
//     of being handed samples;
//   - "the device went away" is AUDCLNT_E_DEVICE_INVALIDATED on one, the stream
//     dropping into its error or unconnected state on the other;
//   - exclusive mode is a request. A backend that cannot honour it runs shared
//     and says so when it starts.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "audio/audio_devices.h"
#include "i18n.h"

namespace cap {

// Interleaved stereo float at the stream's rate, from the backend's thread.
// Stored once, so the indirection costs nothing per sample.
using AudioSinkFn = std::function<void(const float* interleaved, size_t frames)>;

// What an input is for. The backend sizes its device buffer by it, and says its
// failures in the words that fit.
enum class AudioRole {
  Passthrough,  // the card's sound on its way to the speakers: every millisecond counts
  Microphone,   // into the recording only, never played back
};

struct AudioInputEvents {
  // The stream is open, at this rate and with this many channels. Comes before
  // the first onAudio.
  std::function<void(int sampleRate, int channels)> onFormat;
  AudioSinkFn onAudio;
  // Nothing arrived for a while -- about a fifth of a second. Optional; a level
  // meter uses it to fall back.
  std::function<void()> onQuiet;
  // The stream failed after Start returned: the device could not be opened
  // after all, or it went away. Nothing is delivered after this.
  std::function<void(const Said& said)> onFailure;
};

class AudioInput {
 public:
  virtual ~AudioInput() = default;

  // Starts delivering. False when it failed right here, with `error` in the
  // interface language and the log written. A backend that opens the device on
  // its own thread reports what goes wrong there through onFailure instead.
  virtual bool Start(const AudioDeviceInfo& device, AudioRole role, AudioInputEvents events,
                     std::string* error) = 0;
  // Stops and waits until no event runs any more. Harmless when not started.
  virtual void Stop() = 0;
};

// How the output came up.
struct AudioOutputFormat {
  int sampleRate = 0;
  int channels = 0;
  int bufferFrames = 0;  // the device's own buffer
  bool exclusive = false;
};

// The engine's answer for one period.
struct AudioFill {
  bool silent = false;  // play silence, the samples are not written
  float gain = 1.0f;    // applied while converting to the device's format
};

struct AudioOutputEvents {
  // The stream is open. Comes before the first fill.
  std::function<void(const AudioOutputFormat& format)> onStart;
  // Wants `frames` of interleaved stereo float at the output's rate in `out`,
  // from the backend's thread.
  std::function<AudioFill(float* out, size_t frames)> fill;
  // The stream failed after Start returned. Nothing is asked for after this.
  std::function<void(const Said& said)> onFailure;
};

class AudioOutput {
 public:
  virtual ~AudioOutput() = default;

  // Same contract as AudioInput::Start.
  virtual bool Start(const AudioDeviceInfo& device, bool exclusive, AudioOutputEvents events,
                     std::string* error) = 0;
  virtual void Stop() = 0;
};

// The input and output that can open `device`. Which backend that is, the
// device's `backend` says, and only the platform half reads it.
std::unique_ptr<AudioInput> CreateAudioInput(const AudioDeviceInfo& device);
std::unique_ptr<AudioOutput> CreateAudioOutput(const AudioDeviceInfo& device);

}  // namespace cap
