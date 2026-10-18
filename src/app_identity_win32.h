#pragma once

// What this program is called -- and what it used to be called -- on the
// Windows side: the wide name the Windows API takes, the window class, and
// correcting the executable's own name after a rename. The rest of the program
// gets the name and its own files from app_files.h.
//
// Every name the program wears is derived from one string, in app_name.h.
// Renaming the program is therefore two edits: change that one, append the old
// one to kFormerAppNames here. Nothing else in the source spells the name out.
//
// The list of former names is the part that matters after a rename has already
// happened. A build that finds CapView.json next to itself knows that file was
// once its own, and takes it over instead of starting from defaults. That works
// however many names ago it was, which is why the list is a list and not a
// single predecessor -- and it is why the migrator shipped for CapView 3.7 will
// never need replacing: the program itself carries the history.

#include <string>
#include <vector>

#include "app_name.h"

namespace cap {

// The literal, for the handful of places that have to paste the name together
// before there is a program to ask: a resource script, the camera's name in the
// device list. Everywhere else uses kAppName. An empty wide literal in front
// makes the whole concatenation wide.
#define CAP_APP_NAME L"" CAP_APP_NAME_UTF8

inline const wchar_t kAppName[] = CAP_APP_NAME;

// Oldest first. CapView is what this program was called up to and including
// 3.7; 4.0 is that same program under the name it keeps.
inline const wchar_t* const kFormerAppNames[] = {L"CapView"};
inline constexpr size_t kFormerAppNameCount = 1;

// The window class this program registers. Carries the name so two programs
// never collide over it.
std::wstring WindowClassName(const wchar_t* suffix);

// Where the running executable lives. ExeDirectory ends in a backslash.
std::wstring ExePath();
std::wstring ExeDirectory();

// "C:\...\qBlank.exe" -> "qBlank". Works on any path.
std::wstring FileStem(const std::wstring& path);

// A file next to the executable, named after the program: AppFile(L"json")
// gives "C:\...\qBlank.json". The extension comes without its dot.
std::wstring AppFile(const wchar_t* extension);

// The same for a former name, by index into kFormerAppNames.
std::wstring FormerAppFile(size_t index, const wchar_t* extension);

// Moves a file to "<name>.bak" instead of deleting it, replacing an older .bak
// if one is there. Used wherever a file has to get out of the way: what is in
// these files is worth more than the tidiness, and a wrong answer stays one
// rename away from being undone.
bool SetAside(const std::wstring& path);

// Takes over the log a former name left behind, if the current name has none.
// Only the log. Which settings file belongs to this program is not decided by
// its name but by what is in it -- see FindForeignSettings in config.h.
void AdoptFormerLog();

// If the running image carries a name this build no longer answers to, renames
// it and points shortcuts at the new path. Windows allows renaming a running
// executable -- only overwriting is forbidden -- which is the same trick the
// updater uses to replace itself.
//
// This is what makes an ordinary update across a rename work: the updater keeps
// the old file name when it swaps the new build in, and the new build corrects
// the name on its first start. Returns true when something was renamed.
bool AdoptOwnName();

// Points every shortcut aiming at `from` to `to`. Looks on both desktops, in
// both start menus, in quick launch and among the pinned taskbar items. Returns
// how many were changed.
//
// A pinned taskbar item may keep grouping windows under its old identity until
// it is unpinned and pinned again; the shortcut itself starts the right program
// either way. Needs COM to have been initialised on this thread.
int RepointShortcuts(const std::wstring& from, const std::wstring& to);

}  // namespace cap
