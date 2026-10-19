#include "child_process.h"

#include "common_win32.h"
// shellapi.h is a wall of errors without windows.h having been seen first.
#include <shellapi.h>

#include <thread>

#include "text_win32.h"

namespace cap {
namespace {

// One argument, appended the way CommandLineToArgvW will take it apart again.
// Backslashes only need doubling where a quote follows them, which is why this
// cannot be a simple search and replace.
void AppendQuoted(std::wstring* out, const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    *out += arg;
    return;
  }
  *out += L'"';
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out->append(backslashes * 2, L'\\');
      break;
    }
    out->append(arg[i] == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    *out += arg[i];
  }
  *out += L'"';
}

// The program is always quoted, whether it needs it or not: an unquoted path is
// where CreateProcessW starts guessing which of several files with spaces in
// their names was meant.
std::vector<wchar_t> CommandLineFor(const ProcessSpec& spec) {
  std::wstring cmd = L"\"" + ToWide(spec.program) + L"\"";
  for (const std::string& arg : spec.args) {
    cmd += L' ';
    AppendQuoted(&cmd, ToWide(arg));
  }
  std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
  mutableCmd.push_back(L'\0');
  return mutableCmd;
}

// Starts one. `output`, when given, is the child's end of a pipe that takes its
// output and its error stream both, and stays the caller's to close.
bool Spawn(const ProcessSpec& spec, HANDLE output, PROCESS_INFORMATION* pi) {
  STARTUPINFOEXW si = {};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.StartupInfo.wShowWindow = SW_HIDE;
  si.StartupInfo.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  si.StartupInfo.hStdOutput = output ? output : ::GetStdHandle(STD_OUTPUT_HANDLE);
  si.StartupInfo.hStdError = output ? output : ::GetStdHandle(STD_ERROR_HANDLE);

  // Only the handle meant for this child. Inheriting everything inheritable
  // hands it the pipe ends of any other child started at the same moment on
  // another thread, and that copy keeps the other pipe open after its own child
  // has gone, so the thread reading from it never sees the end.
  SIZE_T size = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<uint8_t> storage(size);
  auto* list = (LPPROC_THREAD_ATTRIBUTE_LIST)storage.data();
  const bool listed = output && ::InitializeProcThreadAttributeList(list, 1, 0, &size);
  DWORD flags = CREATE_NO_WINDOW;
  if (listed && ::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &output,
                                            sizeof(HANDLE), nullptr, nullptr)) {
    si.lpAttributeList = list;
    flags |= EXTENDED_STARTUPINFO_PRESENT;
  }

  std::vector<wchar_t> commandLine = CommandLineFor(spec);
  *pi = {};
  const bool ok = ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr,
                                   output != nullptr, flags, nullptr, nullptr, &si.StartupInfo,
                                   pi) != FALSE;
  if (listed) ::DeleteProcThreadAttributeList(list);
  return ok;
}

// Waits, and kills whatever is still running when the time is up.
int WaitFor(HANDLE process, uint32_t timeoutMs) {
  ::WaitForSingleObject(process, timeoutMs);
  DWORD code = 0;
  if (::GetExitCodeProcess(process, &code) && code == STILL_ACTIVE) {
    ::TerminateProcess(process, 1);
    return -1;
  }
  return (int)code;
}

}  // namespace

bool RunAndCollect(const ProcessSpec& spec, std::string* output, int* exitCode,
                   uint32_t timeoutMs) {
  if (output) output->clear();
  if (exitCode) *exitCode = -1;
  if (spec.program.empty()) return false;

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr, writePipe = nullptr;
  if (!::CreatePipe(&readPipe, &writePipe, &sa, 1 << 16)) return false;
  // Only the child may inherit the write end.
  ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  PROCESS_INFORMATION pi = {};
  const bool ok = Spawn(spec, writePipe, &pi);
  ::CloseHandle(writePipe);  // the child owns it now
  if (!ok) {
    ::CloseHandle(readPipe);
    return false;
  }

  // Read on the side while the time runs here. Reading until the end first and
  // only then waiting meant a child that hung without closing its output was
  // never timed out, and took the thread that asked with it.
  std::string captured;
  std::thread reader([readPipe, &captured] {
    char buffer[4096];
    DWORD read = 0;
    while (::ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
      captured.append(buffer, read);
      if (captured.size() > 1 << 20) break;  // a stuck child must not eat memory
    }
  });
  const int code = WaitFor(pi.hProcess, timeoutMs);
  // Gone or killed, its end of the pipe is closed and the reader runs out.
  reader.join();
  ::CloseHandle(readPipe);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);

  if (output) *output = std::move(captured);
  if (exitCode) *exitCode = code;
  return true;
}

bool RunAndWait(const ProcessSpec& spec, int* exitCode, uint32_t timeoutMs) {
  if (exitCode) *exitCode = -1;
  if (spec.program.empty()) return false;

  PROCESS_INFORMATION pi = {};
  if (!Spawn(spec, nullptr, &pi)) return false;

  const int code = WaitFor(pi.hProcess, timeoutMs);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);

  if (exitCode) *exitCode = code;
  return true;
}

// ShellExecuteW and not CreateProcessW: what this starts is not a child of this
// process and inherits nothing from it -- no handles, no console. The build that
// replaces this one has to outlive it cleanly, and a process still holding the
// old one's handles is exactly what would keep the old file from going away.
bool StartAndLetGo(const std::filesystem::path& program, const std::filesystem::path& startIn) {
  if (program.empty()) return false;
  const HINSTANCE result =
      ::ShellExecuteW(nullptr, L"open", program.c_str(), nullptr,
                      startIn.empty() ? nullptr : startIn.c_str(), SW_SHOWNORMAL);
  // Everything up to 32 is one of the old error codes rather than an instance.
  return (INT_PTR)result > 32;
}

}  // namespace cap
