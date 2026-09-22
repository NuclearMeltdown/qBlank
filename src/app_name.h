#pragma once

// What this program is called. The one place the name is spelled out.
//
// Every name the program wears is derived from this string: the executable, the
// settings file, the log, the window class, the folders it suggests for
// recordings, the camera in other programs' device lists. Renaming the program
// is therefore two edits: change the name here, and append the old one to
// kFormerAppNames in app_identity.h.
//
// Only the literal, and nothing else, because a DLL includes this that would
// rather not pull in the standard library for it. The UTF-8 name for the rest of
// the program is AppNameUtf8() in app_files.h; the Windows side derives its wide
// CAP_APP_NAME from this in app_identity.h.
#define CAP_APP_NAME_UTF8 "qBlank"
