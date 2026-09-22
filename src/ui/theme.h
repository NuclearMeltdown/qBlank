#pragma once

#include "config.h"

namespace cap {

// True when Windows is set to a dark app theme.
bool IsSystemDarkMode();

// Resolves Theme::System against the current Windows setting.
bool ResolveDark(Theme theme);

// Applies colours, spacing and rounding to the current ImGui context. The whole
// palette is derived from `accentRgb` (0xRRGGBB): surfaces become desaturated
// tints of it, so switching the accent tints the window background along with
// the controls instead of leaving a blue-grey shell around a violet button.
void ApplyImGuiTheme(bool dark, unsigned accentRgb);

// Loads Segoe UI at the given size into the ImGui atlas, falling back to the
// built-in font. Must run before the backend builds its font texture.
void LoadUiFont(float sizePixels);

// Background the video area is cleared to. Also a tint of the accent, but far
// darker than any UI surface so it never competes with the picture.
void GetBackgroundColor(bool dark, unsigned accentRgb, float out[4]);

}  // namespace cap
