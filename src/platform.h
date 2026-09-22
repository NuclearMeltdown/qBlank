#pragma once

// The small things the program asks of the system, each of which would
// otherwise be one Windows call sitting in the middle of code that has no
// other reason to know about Windows. None of them is big enough to deserve a
// header of its own; together they are the desktop this program runs on.
//
// The Windows half is platform_win32.cpp.

#include <string>
#include <vector>

namespace cap {

// True when the desktop is set to a dark application theme. A system with no
// opinion answers true: this program is meant for a darkened room.
bool SystemPrefersDark();

// Font files for the interface, best first, as UTF-8 paths. Only files that
// exist are named. Empty means the built-in font has to do.
std::vector<std::string> UiFontFiles();

}  // namespace cap
