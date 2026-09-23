#pragma once

// Handing something to the desktop: a page, a folder, a file. Every one of
// these is the same sentence -- "open this the way the user has it set up" --
// and the answer is somebody else's program, not ours. And leaving a way back
// to this program there: a shortcut in the start menu or on the desktop.
//
// None of the opening waits for anything. The call succeeds when the request
// was accepted, not when the other program has finished doing it, and a false
// only means it could not be handed over at all. The shortcuts are plain files
// and done when the call returns.
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

// A shortcut to this program where the desktop keeps them. "This program" is
// the copy that is running: a second copy elsewhere, or this one after it was
// moved, gets a shortcut of its own.
enum class ShortcutPlace { StartMenu, Desktop };

// Whether the shortcut named after the program is there and starts this copy.
// One of that name starting another copy does not count. Reads a file, so it
// is asked once and remembered, not every frame.
bool HasShortcut(ShortcutPlace place);

// Puts it there, replacing one of the same name.
bool CreateShortcut(ShortcutPlace place);

// Takes it away again -- only one that starts this copy. False when there was
// none or it could not be removed.
bool RemoveShortcut(ShortcutPlace place);

}  // namespace cap
