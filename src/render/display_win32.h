#pragma once

// The Windows half of display.h, for the renderer's passes: they draw with the
// same device into the same back buffer.

#include <d3d11.h>

#include "render/display.h"

namespace cap {

// Null while the display is not initialised.
ID3D11Device* NativeDevice(const Display& display);
ID3D11DeviceContext* NativeContext(const Display& display);
ID3D11RenderTargetView* NativeBackBuffer(const Display& display);

// `width` by `height` of `source`, from its top left, queued for a still --
// for the passes and the back buffer alike. Null when it is smaller than that
// or in a format a still cannot hold. Map with DO_NOT_WAIT, see still_copy.h.
std::unique_ptr<StillCopy> QueueStillCopy(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                                          int width, int height);

// What the screen the window mostly sits on can do, for both backends. Cheap
// while nothing changed: the screens are walked again only when the window
// moved to another one or the system said they changed.
Display::DisplayCapability ScreenCapability(HWND window);

}  // namespace cap
