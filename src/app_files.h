#pragma once

// What the program is called and which files are its own, in the terms the rest
// of the program works in: the name as UTF-8, files as std::filesystem::path.
//
// The Windows side of the same -- the wide name, the window class, correcting
// the executable's name after a rename -- is app_identity.h. The name itself is
// spelled out once, in app_name.h.

#include <filesystem>
#include <string>

#include "app_name.h"

namespace cap {

// The program's name, for the places that speak UTF-8 -- window titles drawn by
// the interface, log lines, message text.
const std::string& AppNameUtf8();

// The folder the running executable lives in. The real file's, not that of a
// link it was started through: settings, log, ffmpeg and the updater all belong
// next to the program itself.
std::filesystem::path ExeFolder();

// A file next to the executable, named after the program: OwnFile("json") gives
// "...\qBlank.json". The extension comes without its dot.
std::filesystem::path OwnFile(const char* extension);

// The running program's own file. Not OwnFile with the platform's extension for
// a program: after a rename the image on disk still carries the old name for one
// more start, and this is the file that is really running -- the one the updater
// has to replace.
std::filesystem::path OwnProgramFile();

// Moves a file to "<name>.bak" instead of deleting it, replacing an older .bak
// if one is there. Used wherever a file has to get out of the way: what is in
// these files is worth more than the tidiness, and a wrong answer stays one
// rename away from being undone.
bool SetFileAside(const std::filesystem::path& path);

// Which name the settings now in use were written under, empty when they were
// always under the current one. Set by whoever adopts them, read by anything
// that runs later in startup.
//
// It matters because settings can mean different things under different names:
// an empty folder setting means "next to the videos, in a folder called after
// the program", and that folder does not rename itself.
const std::string& AdoptedFrom();
void SetAdoptedFrom(const std::string& name);

}  // namespace cap
