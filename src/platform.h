#pragma once

// The small things the program asks of the system, each of which would
// otherwise be one Windows call sitting in the middle of code that has no
// other reason to know about Windows. None of them is big enough to deserve a
// header of its own; together they are the desktop this program runs on.
//
// The Windows half is platform_win32.cpp.

#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace cap {

// True when the desktop is set to a dark application theme. A system with no
// opinion answers true: this program is meant for a darkened room.
bool SystemPrefersDark();

// Font files for the interface, best first, as UTF-8 paths. Only files that
// exist are named. Empty means the built-in font has to do.
std::vector<std::string> UiFontFiles();

// The program's own icon, as straight RGBA and square, for the empty state.
// `size` is what would suit; what came back is in `width` and `height`, because
// a system that keeps icons in fixed sizes hands over the nearest one it has.
// Empty when there is no icon to be had.
std::vector<uint8_t> AppIconRgba(int size, int* width, int* height);

// Where the user lives, as an upper case two letter country code. Empty when
// the system will not say. This is the residence, not the language: they are
// two different settings, and they part company as soon as somebody runs an
// English system and stays in Germany.
std::string UserCountryCode();

// A date and time the way the system is set to show them, without seconds:
// "08.09.2026 21:14" in one place, "9/8/2026 9:14 PM" in another. Empty for a
// LocalTime that is all zero.
std::string ShortDateAndTime(const LocalTime& when);

// Asks the system not to blank the screen or go to sleep while a capture is
// running. Called repeatedly; the last call decides.
void KeepDisplayAwake(bool keep);

// A thread this program starts itself, which will go on to reach the system
// through one of the backends, announces itself for as long as it runs. Where a
// system has no such rule this is an empty object, and where it has one, not
// holding it is the kind of mistake that shows up as a call failing on one
// machine and working on the next.
//
// The threads that frames and audio arrive on belong to the backends and look
// after themselves; this is for the ones above that line.
class SystemThreadScope {
 public:
  SystemThreadScope();
  ~SystemThreadScope();
  SystemThreadScope(const SystemThreadScope&) = delete;
  SystemThreadScope& operator=(const SystemThreadScope&) = delete;

 private:
  bool held_ = false;
};

// Asks for the finest timer resolution the system offers, and gives it back.
// The wait in the render loop and the second field of a bob deinterlaced frame
// both need better than the default. Paired, and not nested.
void BeginPreciseTiming();
void EndPreciseTiming();

// Before any window exists: tell the system this program places its pixels
// itself, so a scaled display does not hand it a stretched picture.
void MakeProcessDpiAware();

// A windowed program started from a terminal has no output of its own. This
// borrows the terminal that started it, for the two command line modes. Does
// nothing when there is none.
void UseParentConsole();

}  // namespace cap
