#pragma once

// Starting another program.
//
// qBlank starts two foreign programs, both briefly: the system's tar, to ask
// what it can unpack and then to unpack a downloaded ffmpeg, and the build that
// just replaced this one. ffmpeg itself runs inside this process as libraries.
//
// Arguments are a vector, never one string. Every system has its own rules for
// turning a list of arguments into what the child actually receives, and a path
// with a space in it is exactly what those rules get wrong. One argument here is
// one argument in the child; the quoting belongs to the backend.
//
// The Windows half is child_process_win32.cpp.

#include <cstdint>
#include <filesystem>
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

}  // namespace cap
