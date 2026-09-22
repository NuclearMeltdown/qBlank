#pragma once

// The Windows half of common.h: what code that talks to Windows directly needs
// on almost every line. Windows code only -- a header the rest of the program
// reads has no business including it.

#include <windows.h>
#include <wrl/client.h>

#include <string>

#include "common.h"

// Short alias -- ComPtr shows up on almost every line of the DirectShow / D3D code.
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

namespace cap {

// Logs the call site and returns hr, so it can be used inline in a condition.
HRESULT LogHrFailure(HRESULT hr, const char* expr, const char* file, int line);

#define CAP_HR(expr) ::cap::LogHrFailure((expr), #expr, __FILE__, __LINE__)

// Human readable HRESULT, e.g. "0x80070002 (The system cannot find the file
// specified)". The text comes from Windows: in English where T() speaks English
// and Windows has the English text, otherwise in the language of the system.
std::string HrToString(HRESULT hr);

// The same, always asking for English first -- for log lines written directly
// rather than through CAP_SAID.
std::string HrToEnglish(HRESULT hr);

// RAII wrapper for CoInitializeEx on the calling thread.
class ComScope {
 public:
  explicit ComScope(DWORD model = COINIT_APARTMENTTHREADED);
  ~ComScope();
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  bool ok() const { return initialized_; }

 private:
  bool initialized_ = false;
};

}  // namespace cap
