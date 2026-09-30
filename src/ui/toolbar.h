#pragma once

// An optional strip of buttons along the top of the picture.
//
// Everything here is also on a key and in the right-click menu. It exists for
// people who would rather see the controls than remember them, which is a
// reasonable preference and not one the program should make them pay for: the
// bar reserves its own height rather than covering the picture, and can be
// turned off entirely.
//
// The icons are drawn with lines and circles rather than loaded from an icon
// font. A font would be several hundred kilobytes against an executable of
// 1.6 MB, for a dozen glyphs.

#include <string>

#include "common.h"
#include "config.h"

namespace cap {

struct ToolbarState {
  bool recording = false;
  double recordSeconds = 0.0;
  bool muted = false;
  float volume = 1.0f;   // 0..1
  bool canRecord = true;  // false while ffmpeg is missing or nothing is running
  // The key that brings the bar back, named in the Hide tooltip. Only read on
  // hover, so the key name is not built every frame.
  const HotkeyBinding* toggleKey = nullptr;
};

// What the user pressed, if anything. One per frame at most.
enum class ToolbarAction {
  None,
  ToggleRecording,
  Screenshot,
  Settings,
  OpenRecordFolder,
  OpenScreenshotFolder,
  ToggleMute,
  Hide,
};

struct ToolbarResult {
  ToolbarAction action = ToolbarAction::None;
  float volume = -1.0f;  // >= 0 when the slider moved
};

// Height in client pixels, so the caller can keep the picture clear of it.
float ToolbarHeight();

// Draws the bar across the top of the viewport.
ToolbarResult DrawToolbar(const ToolbarState& state, unsigned accentRgb);

}  // namespace cap
