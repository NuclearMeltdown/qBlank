#include "child_process.h"

#include "common_win32.h"

#include <algorithm>
#include <thread>

#include "i18n.h"
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

// Both concrete streams, seen from the process starter: it needs the end the
// child gets, and only the standard input stream has one.
class Win32Stream : public ChildStream {
 public:
  const std::string& name() const override { return name_; }

  bool Write(const void* data, size_t size) override {
    const uint8_t* at = (const uint8_t*)data;
    size_t written = 0;
    while (written < size) {
      // Pipe writes are capped: a whole 4K frame in one call is a lot to ask of
      // a pipe buffer, and the loop has to exist anyway.
      const DWORD want = (DWORD)std::min<size_t>(size - written, 1u << 20);
      DWORD chunk = 0;
      if (!WriteChunk(at + written, want, &chunk) || chunk == 0) return false;
      written += chunk;
    }
    return true;
  }

  // The handle the child inherits, or null when there is none to pass on.
  virtual HANDLE childEnd() const { return nullptr; }

 protected:
  virtual bool WriteChunk(const void* data, DWORD size, DWORD* wrote) = 0;

  std::string name_;
};

// The child's standard input: an ordinary anonymous pipe, one end inheritable.
class InputStream final : public Win32Stream {
 public:
  ~InputStream() override {
    Close();
    if (childEnd_) ::CloseHandle(childEnd_);
  }

  bool Open(size_t bufferBytes) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!::CreatePipe(&childEnd_, &ours_, &sa, (DWORD)bufferBytes)) return false;
    // Only the child's end may be inherited.
    ::SetHandleInformation(ours_, HANDLE_FLAG_INHERIT, 0);
    return true;
  }

  Connect WaitForChild(uint32_t) override { return Connect::Connected; }

  void Close() override {
    if (!ours_) return;
    ::CloseHandle(ours_);
    ours_ = nullptr;
  }

  HANDLE childEnd() const override { return childEnd_; }

  // Called once the child has it: from then on the child owns that end, and
  // holding on to a copy here would keep the pipe open after the child died.
  void ForgetChildEnd() {
    if (!childEnd_) return;
    ::CloseHandle(childEnd_);
    childEnd_ = nullptr;
  }

 protected:
  bool WriteChunk(const void* data, DWORD size, DWORD* wrote) override {
    return ::WriteFile(ours_, data, size, wrote, nullptr) != FALSE;
  }

 private:
  HANDLE childEnd_ = nullptr;
  HANDLE ours_ = nullptr;
};

// A stream the child opens by path. ffmpeg cannot take a second anonymous pipe
// -- only stdin arrives as an inheritable standard handle -- so anything past
// the first stream has to be a named pipe it opens itself.
//
// Overlapped, and not for the writing: waiting for the child to connect has to
// be interruptible, or an ffmpeg that never starts leaves the thread that waits
// for it stuck and hangs the whole shutdown.
class NamedStream final : public Win32Stream {
 public:
  ~NamedStream() override {
    Close();
    if (event_) ::CloseHandle(event_);
  }

  bool Open(const char* tag, size_t bufferBytes) {
    event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event_) return false;
    const std::wstring path = std::wstring(L"\\\\.\\pipe\\") + ToWide(tag) + L"_" +
                              std::to_wstring(::GetCurrentProcessId());
    pipe_ = ::CreateNamedPipeW(path.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_BYTE | PIPE_WAIT, 1, (DWORD)bufferBytes,
                               (DWORD)bufferBytes, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) {
      pipe_ = nullptr;
      return false;
    }
    name_ = ToUtf8(path);
    return true;
  }

  Connect WaitForChild(uint32_t timeoutMs) override {
    if (!pipe_) return Connect::Failed;
    if (connected_) return Connect::Connected;

    if (!pending_) {
      ov_ = {};
      ov_.hEvent = event_;
      if (::ConnectNamedPipe(pipe_, &ov_)) {
        connected_ = true;
        return Connect::Connected;
      }
      const DWORD err = ::GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        connected_ = true;
        return Connect::Connected;
      }
      if (err != ERROR_IO_PENDING) return Connect::Failed;
      pending_ = true;
    }

    if (::WaitForSingleObject(event_, timeoutMs) != WAIT_OBJECT_0) return Connect::Waiting;
    pending_ = false;
    connected_ = true;
    return Connect::Connected;
  }

  void Close() override {
    if (!pipe_) return;
    if (pending_) ::CancelIo(pipe_);
    ::CloseHandle(pipe_);
    pipe_ = nullptr;
    pending_ = false;
    connected_ = false;
  }

 protected:
  // A named pipe opened overlapped has to be written overlapped, so the write is
  // started and then waited for. The caller sees a plain blocking write, which
  // is all it ever wanted.
  bool WriteChunk(const void* data, DWORD size, DWORD* wrote) override {
    if (!pipe_) return false;
    ::ResetEvent(event_);
    OVERLAPPED wov = {};
    wov.hEvent = event_;
    if (::WriteFile(pipe_, data, size, wrote, &wov)) return true;
    if (::GetLastError() != ERROR_IO_PENDING) return false;
    return ::GetOverlappedResult(pipe_, &wov, wrote, TRUE) != FALSE;
  }

 private:
  HANDLE pipe_ = nullptr;
  HANDLE event_ = nullptr;
  OVERLAPPED ov_ = {};
  bool pending_ = false;
  bool connected_ = false;
};

// Starts one, with whatever standard handles were asked for. `stdOut` and
// `stdErr` are the child's ends and stay the caller's to close.
bool Spawn(const ProcessSpec& spec, HANDLE stdIn, HANDLE stdOut, HANDLE stdErr,
           PROCESS_INFORMATION* pi) {
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = stdIn ? stdIn : ::GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = stdOut ? stdOut : ::GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = stdErr ? stdErr : ::GetStdHandle(STD_ERROR_HANDLE);

  std::vector<wchar_t> commandLine = CommandLineFor(spec);
  *pi = {};
  return ::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, pi) != FALSE;
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

// ------------------------------------------------------------ running to the end

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

  // Output and error go into one pipe, because the interesting half of what
  // ffmpeg says is on the error one and the order the two were said in is worth
  // keeping.
  PROCESS_INFORMATION pi = {};
  const bool ok = Spawn(spec, nullptr, writePipe, writePipe, &pi);
  ::CloseHandle(writePipe);  // the child owns it now
  if (!ok) {
    ::CloseHandle(readPipe);
    return false;
  }

  std::string captured;
  char buffer[4096];
  DWORD read = 0;
  while (::ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    captured.append(buffer, read);
    if (captured.size() > 1 << 20) break;  // a stuck child must not eat memory
  }
  ::CloseHandle(readPipe);

  const int code = WaitFor(pi.hProcess, timeoutMs);
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
  if (!Spawn(spec, nullptr, nullptr, nullptr, &pi)) return false;

  const int code = WaitFor(pi.hProcess, timeoutMs);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);

  if (exitCode) *exitCode = code;
  return true;
}

// ------------------------------------------------------------------- streams

std::unique_ptr<ChildStream> MakeInputStream(size_t bufferBytes) {
  auto stream = std::make_unique<InputStream>();
  if (!stream->Open(bufferBytes)) return nullptr;
  return stream;
}

std::unique_ptr<ChildStream> MakeNamedStream(const char* tag, size_t bufferBytes) {
  auto stream = std::make_unique<NamedStream>();
  if (!stream->Open(tag, bufferBytes)) return nullptr;
  return stream;
}

// ------------------------------------------------------------- a living child

struct ChildProcess::Impl {
  HANDLE process = nullptr;
  HANDLE errorRead = nullptr;
  std::thread errorThread;

  void ReadErrors(ErrorLineFn onLine) {
    std::string pending;
    char buffer[1024];
    DWORD read = 0;
    while (::ReadFile(errorRead, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
      pending.append(buffer, read);
      size_t nl;
      while ((nl = pending.find_first_of("\r\n")) != std::string::npos) {
        const std::string line = Trim(pending.substr(0, nl));
        pending.erase(0, nl + 1);
        if (!line.empty()) onLine(line);
      }
      // A child that says one very long thing without a line break is not worth
      // buffering further.
      if (pending.size() > 4096) pending.clear();
    }
  }
};

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess() { WaitOrKill(0); }

bool ChildProcess::Start(const ProcessSpec& spec, ChildStream* input, ErrorLineFn onErrorLine,
                         std::string* error) {
  if (impl_->process) return ReportError(error, CAP_SAID("a child is already running"));
  if (spec.program.empty()) return ReportError(error, CAP_SAID("no program to start"));

  auto* win32Input = dynamic_cast<InputStream*>(input);
  if (input && !win32Input) return ReportError(error, CAP_SAID("not a standard input stream"));

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  // Without this pipe a child that fails leaves nothing but an exit code. Not
  // getting one is survivable, so a failure here only costs the explanation.
  HANDLE errorWrite = nullptr;
  if (onErrorLine && ::CreatePipe(&impl_->errorRead, &errorWrite, &sa, 1 << 16)) {
    ::SetHandleInformation(impl_->errorRead, HANDLE_FLAG_INHERIT, 0);
  }

  PROCESS_INFORMATION pi = {};
  const bool ok = Spawn(spec, win32Input ? win32Input->childEnd() : nullptr, nullptr, errorWrite,
                        &pi);
  if (win32Input) win32Input->ForgetChildEnd();
  if (errorWrite) ::CloseHandle(errorWrite);
  if (!ok) {
    if (impl_->errorRead) {
      ::CloseHandle(impl_->errorRead);
      impl_->errorRead = nullptr;
    }
    return ReportError(error, CAP_SAID(T("Prozess konnte nicht gestartet werden: ",
                                         "Could not start the process: ") + spec.program));
  }

  ::CloseHandle(pi.hThread);
  impl_->process = pi.hProcess;
  if (impl_->errorRead) {
    impl_->errorThread = std::thread([this, onErrorLine] { impl_->ReadErrors(onErrorLine); });
  }
  return true;
}

bool ChildProcess::started() const { return impl_->process != nullptr; }

int ChildProcess::WaitOrKill(uint32_t timeoutMs) {
  int code = 0;
  if (impl_->process) {
    code = WaitFor(impl_->process, timeoutMs);
    ::CloseHandle(impl_->process);
    impl_->process = nullptr;
  }

  // The reader ends by itself once the child's end of the pipe is gone, which is
  // why it is joined after the wait and not before it.
  if (impl_->errorThread.joinable()) impl_->errorThread.join();
  if (impl_->errorRead) {
    ::CloseHandle(impl_->errorRead);
    impl_->errorRead = nullptr;
  }
  return code;
}

}  // namespace cap
