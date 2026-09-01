// Running a child process and capturing what it prints.
//
// Like touch_win32, this deliberately never includes raylib.h, because
// <windows.h> also declares Rectangle, CloseWindow and ShowCursor.
#pragma once

#include <string>

namespace sb::proc {

struct Result {
  bool        started = false;   // false if the executable could not be launched
  bool        timedOut = false;
  int         exitCode = -1;
  std::string output;            // stdout and stderr, interleaved
};

// Runs `commandLine` with no console window and waits for it, up to
// `timeoutMs`. On timeout the child is killed and `timedOut` is set.
Result Run(const std::string& commandLine, int timeoutMs);

// Quotes a path for use as one argument of a command line.
std::string Quote(const std::string& value);

}  // namespace sb::proc
