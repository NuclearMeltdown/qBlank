#pragma once

// Starting another program, and feeding it while it runs.
//
// qBlank starts exactly one foreign program -- ffmpeg -- but it starts it in
// three shapes: a short question whose answer is read back (which encoders does
// this machine have?), a conversion that is started and waited for (write this
// screenshot, remux this file), and a long lived encoder that is fed raw frames
// and raw audio for as long as a recording runs.
//
// Arguments are a vector, never one string. Every system has its own rules for
// turning a list of arguments into what the child actually receives, and a path
// with a space in it is exactly what those rules get wrong. One argument here is
// one argument in the child; the quoting belongs to the backend.
//
// The Windows half is process_win32.cpp.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cap {

// What to start: a path to a program, and one string per argument. Both are
// UTF-8.
struct ProcessSpec {
  std::string program;
  std::vector<std::string> args;

  void Add(std::string arg) { args.push_back(std::move(arg)); }
  void Add(std::string a, std::string b) {
    args.push_back(std::move(a));
    args.push_back(std::move(b));
  }
};

// Runs it to the end. `output` collects what it wrote to its output and its
// error stream together, in the order it arrived, capped inside so a child that
// never stops talking cannot eat memory.
//
// Returns false only when it could not be started at all. A program that ran and
// failed returns true with a non-zero `exitCode`, which is -1 when it had to be
// killed after `timeoutMs`. Either pointer may be null.
bool RunAndCollect(const ProcessSpec& spec, std::string* output, int* exitCode,
                   uint32_t timeoutMs);

// The same without reading anything back.
bool RunAndWait(const ProcessSpec& spec, int* exitCode, uint32_t timeoutMs);

// Starts a program and lets go of it: it goes on running after this program has
// ended, and nothing of it is read back. No arguments, because the one thing
// started this way is the build that just replaced this one -- and none of the
// quoting rules above would be worth keeping half of.
//
// `startIn` is the folder the new program starts in. It matters: a program that
// looks for its settings next to itself has to be told where itself is.
//
// False means it did not start. True only means it was handed over; whether it
// then ran is not something this can wait around to find out.
bool StartAndLetGo(const std::filesystem::path& program, const std::filesystem::path& startIn);

// A stream of bytes into a running child: this program writes, the child reads.
//
// Streams exist before the child does, because a child is told where to find one
// as a command line argument. `name()` is what to pass it, and is empty for the
// one stream that becomes its standard input instead.
//
// One stream belongs to one thread. Writes block while the child is behind, and
// that is where the waiting belongs: a recording that outruns its encoder has to
// slow down, not silently leave out what it already promised to write.
class ChildStream {
 public:
  virtual ~ChildStream() = default;

  // What the child has to be given to open this stream. Empty for the standard
  // input stream, which the child does not have to open at all.
  virtual const std::string& name() const = 0;

  enum class Connect { Connected, Waiting, Failed };

  // Waits up to `timeoutMs` for the child to open its end, and is called again
  // while it answers Waiting. A short timeout per call is the point: a child
  // that never starts must not be able to wedge the shutdown. The standard input
  // stream is connected the moment the child starts and answers at once.
  virtual Connect WaitForChild(uint32_t timeoutMs) = 0;

  // Writes all of it, blocking until it is gone. False means the other end is
  // no longer there.
  virtual bool Write(const void* data, size_t size) = 0;

  // Closing is how a child is told the stream ended. It is not a formality:
  // ffmpeg writes the container index when its inputs close, and a file whose
  // encoder was killed before that point does not play at all.
  virtual void Close() = 0;
};

// The stream that becomes the child's standard input. It is handed to
// ChildProcess::Start and has to outlive the wait.
std::unique_ptr<ChildStream> MakeInputStream(size_t bufferBytes);

// A stream the child will open by the name this hands back. `tag` only has to
// tell this program's own streams apart from each other. Null when the system
// would not make one.
std::unique_ptr<ChildStream> MakeNamedStream(const char* tag, size_t bufferBytes);

// A child that keeps running while this program feeds it.
class ChildProcess {
 public:
  ChildProcess();
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // One line of whatever the child wrote to its error stream, from a thread of
  // this class's own. ffmpeg says everything there, and a program with no
  // console has nowhere for it to go -- without this a failed encode leaves
  // nothing but an exit code, which is how the first version of this managed to
  // say nothing at all about why it produced no file.
  using ErrorLineFn = std::function<void(const std::string& line)>;

  // Starts it. `input`, when given, becomes its standard input and stays owned
  // by the caller. `onErrorLine`, when given, is called from the reader thread
  // until the child closes its end.
  bool Start(const ProcessSpec& spec, ChildStream* input, ErrorLineFn onErrorLine,
             std::string* error);

  bool started() const;

  // Waits up to `timeoutMs` for it to end and kills it when it does not, then
  // joins the error reader. Returns its exit code, or -1 when it had to be
  // killed; 0 when nothing was started. Leaves this ready to start again.
  int WaitOrKill(uint32_t timeoutMs);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cap
