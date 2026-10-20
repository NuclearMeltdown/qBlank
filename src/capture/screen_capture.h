#pragma once

// The screen as the source: one monitor's picture as the desktop shows it, the
// mouse pointer drawn in or not. Nothing about a capture card applies to it.
//
// The pictures stay on the GPU where they can: three textures on the monitor's
// adapter, shared with the renderer (shared_frames.h), and the frame buffer
// says only which of them is the newest. A renderer on another adapter, or one
// that cannot open shared textures, gets them as RGB32 bytes through the frame
// buffer instead, like a card's.
//
// Neutral; the capture is the platform's (screen_capture_win32.cpp).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "capture/frame_buffer.h"

namespace cap {

class SharedFrames;

struct ScreenInfo {
  std::string id;  // the system's name for the monitor, kept in the config
  int width = 0;   // in pixels
  int height = 0;
  bool primary = false;
};

// In the order the monitors are numbered elsewhere (EnumerateDisplays).
std::vector<ScreenInfo> ListScreens();

class ScreenCapture {
 public:
  virtual ~ScreenCapture() = default;

  // Starts on screen `id`, or the primary one when that is empty or gone, for
  // a renderer on adapter `adapter` (VideoRenderer::sharedAdapter, 0 for bytes
  // only). On failure `error` holds a message for the user.
  virtual bool Start(const std::string& id, bool cursor, uint64_t adapter, std::string* error) = 0;
  virtual void Stop() = 0;
  virtual bool running() const = 0;

  virtual void SetCursor(bool on) = 0;

  // The screen Start ended up on, and the adapter it was asked for.
  virtual const std::string& screen() const = 0;
  virtual uint64_t adapter() const = 0;

  // Where the frames arrive. Null while stopped.
  virtual FrameBuffer* frames() = 0;
  // The textures the frames are in, or null when they come as bytes.
  virtual std::shared_ptr<SharedFrames> sharedFrames() const = 0;

  // True once the screen changed its size or went away: what Start set up no
  // longer fits, and it wants a Stop and a Start. `error` then says why, when
  // there is something to tell the user.
  virtual bool needsRestart(std::string* error) const = 0;
};

std::unique_ptr<ScreenCapture> CreateScreenCapture();

}  // namespace cap
