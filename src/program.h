#pragma once

// The program itself: the housekeeping before the first file is read, the two
// command line modes, and the run of the application. Everything around it is
// the system's business and lives in main_win32.cpp -- what the entry point is
// called, what the process has to announce about itself before any window
// exists, how a command line arrives, and correcting the executable's own name
// after a rename.

#include <string>
#include <vector>

namespace cap {

// Runs the program and returns the code the process should end with.
// `arguments` are the words of the command line after the program's own name,
// as UTF-8.
int RunProgram(const std::vector<std::string>& arguments);

}  // namespace cap
