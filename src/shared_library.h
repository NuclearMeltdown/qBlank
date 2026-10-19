#pragma once

// A library opened at runtime by its full path, and the functions in it.
//
// For what the program uses only when it is there and does not ship itself:
// the ffmpeg libraries the downloader fetches.
//
// The Windows half is shared_library_win32.cpp.

#include <filesystem>
#include <string>

namespace cap {

// What the system calls version `major` of library `base`: "avutil-61.dll" on
// Windows.
std::string SharedLibraryFileName(const char* base, int major);

// Opens `file`. What it needs in turn is looked for next to it and among the
// system's own libraries, nowhere else: not in the working folder, not on
// PATH. Null when it cannot be opened, with `error` saying why.
void* OpenSharedLibrary(const std::filesystem::path& file, std::string* error);

// Only for a library nothing has called into yet. One that is in use stays
// open for good: whatever looked a function up may still be running it.
void CloseSharedLibrary(void* library);

// The address of `name` in `library`, null when it has none.
void* SharedLibrarySymbol(void* library, const char* name);

}  // namespace cap
