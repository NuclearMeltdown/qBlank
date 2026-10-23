#pragma once

// What a screen's pictures mean once Windows runs it in HDR, for the screen's
// and a window's capture alike. Such a desktop is composed in linear scRGB,
// 1.0 at eighty nits, with SDR white wherever the user's slider puts it.

#include "common_win32.h"

namespace cap {

struct MonitorHdr {
  bool on = false;          // the screen runs HDR
  float whiteNits = 80.0f;  // where SDR white sits on it
  float peakNits = 0.0f;    // the brightest it says it shows
};

// Whether the capture has to start anew: the screen went in or out of HDR, or
// its white moved.
inline bool HdrMoved(const MonitorHdr& was, const MonitorHdr& now) {
  return was.on != now.on || was.whiteNits != now.whiteNits;
}

// The screen of this GDI name as its display path has it, without the peak:
// cheap enough to ask once a second.
MonitorHdr DisplayHdr(const wchar_t* device);

// The same with the peak, for the monitor.
MonitorHdr HdrOf(HMONITOR monitor);

// The display path that shows the screen of this GDI name.
bool DisplayPathOf(const wchar_t* device, DISPLAYCONFIG_PATH_INFO* path);

}  // namespace cap
