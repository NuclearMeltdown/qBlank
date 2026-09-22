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

}  // namespace cap
