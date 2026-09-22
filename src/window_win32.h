#pragma once

// The Windows half of window.h, for code that hands a window to a Windows API
// itself: a swap chain, the clipboard, a file dialog.

#include <windows.h>

#include "window.h"

namespace cap {

// Null while the window does not exist.
HWND NativeWindow(const Window& window);

// How the program was asked to show itself -- the nCmdShow of WinMain, which a
// shortcut set to "minimised" changes. The main window's first showing follows
// it unless it is to come up maximised.
void SetStartupShowCommand(int showCmd);

}  // namespace cap
