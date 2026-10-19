// The Windows half of shared_library.h.

#include "shared_library.h"

#include "common_win32.h"

namespace cap {

std::string SharedLibraryFileName(const char* base, int major) {
  return Format("%s-%d.dll", base, major);
}

void* OpenSharedLibrary(const std::filesystem::path& file, std::string* error) {
  // The library's own folder, then System32. The default order would also try
  // the program's folder and PATH, where an unrelated copy of a dependency
  // could be lying.
  HMODULE module = ::LoadLibraryExW(file.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) {
    const HRESULT hr = HRESULT_FROM_WIN32(::GetLastError());
    const std::string name = PathToUtf8(file.filename());
    ReportError(error, CAP_SAID(Format(T("%s lässt sich nicht laden: %s", "%s cannot be loaded: %s"),
                                       name.c_str(), HrToString(hr).c_str())));
    return nullptr;
  }
  return module;
}

void CloseSharedLibrary(void* library) {
  if (library) ::FreeLibrary((HMODULE)library);
}

void* SharedLibrarySymbol(void* library, const char* name) {
  return library ? (void*)::GetProcAddress((HMODULE)library, name) : nullptr;
}

}  // namespace cap
