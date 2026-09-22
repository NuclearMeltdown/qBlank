#pragma once

// Handing something to the desktop: a page, a folder, a file. Every one of
// these is the same sentence -- "open this the way the user has it set up" --
// and the answer is somebody else's program, not ours.
//
// Nothing here waits for anything. The call succeeds when the request was
// accepted, not when the other program has finished doing it, and a false only
// means it could not be handed over at all.
//
// The Windows half is desktop_win32.cpp.

#include <filesystem>
#include <string>

namespace cap {

// A http or https address in the user's browser.
bool OpenUrl(const std::string& url);

// A folder in the file manager.
bool OpenFolder(const std::filesystem::path& folder);

// A file in whatever is registered for it. A log opens in a text editor.
bool OpenFile(const std::filesystem::path& file);

// The folder the file is in, with the file itself picked out. Falls back to
// showing the plain folder when the desktop cannot do better.
bool ShowFileInFolder(const std::filesystem::path& file);

// The last thing said by a program that cannot start: no window of ours exists
// yet, so this has to be the desktop's own way of showing a message. Blocks
// until the user has read it.
void ShowFatalMessage(const std::string& text);

}  // namespace cap
