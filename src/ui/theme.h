#pragma once

#include "config.h"

namespace cap {

// Resolves Theme::System against what the desktop is set to.
bool ResolveDark(Theme theme);

// Applies colours, spacing and rounding to the current ImGui context. The whole
// palette is derived from `accentRgb` (0xRRGGBB): surfaces become desaturated
// tints of it, so switching the accent tints the window background along with
// the controls instead of leaving a blue-grey shell around a violet button.
// `scale` is the monitor's DPI scale; sizes and font follow it. Safe to call
// again whenever any of the three changes.
void ApplyImGuiTheme(bool dark, unsigned accentRgb, float scale);

// Size the UI font is loaded at, in pixels at 100 %. The DPI scale is applied
// on top by ApplyImGuiTheme.
constexpr float kUiFontSize = 17.0f;

// Loads Segoe UI at kUiFontSize into the ImGui atlas, falling back to the
// built-in font. Must run before the backend builds its font texture.
void LoadUiFont();

// Background the video area is cleared to. Also a tint of the accent, but far
// darker than any UI surface so it never competes with the picture.
void GetBackgroundColor(bool dark, unsigned accentRgb, float out[4]);

}  // namespace cap
