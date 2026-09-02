// Running a child process and capturing what it prints.
//
// Like touch_win32, this deliberately never includes raylib.h, because
// <windows.h> also declares Rectangle, CloseWindow and ShowCursor.
#pragma once

#include <cstddef>
#include <functional>
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

// A child whose output is read *while it runs* rather than collected at the
// end. Run() is the right shape for a compiler, which says a little and stops;
// this is the right shape for a decoder, which says a great deal and would
// otherwise have to finish -- and be held in memory whole -- before anything
// could be shown.
//
// The handles are kept as void* so that <windows.h> stays out of this header,
// for the same reason the rest of the file avoids it.
class Pipe {
 public:
  Pipe() = default;
  ~Pipe();
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  bool Start(const std::string& commandLine);
  // Reads exactly `bytes`, waiting for them. Returns fewer only when the child
  // has stopped writing or `abort` asked it to give up -- so a short read means
  // the stream is over, and the caller has no partial frame to guess about.
  //
  // `abort` is polled rather than the handle being closed from another thread,
  // so a reader can be stopped without two threads touching one handle.
  std::size_t Read(unsigned char* into, std::size_t bytes,
                   const std::function<bool()>& abort = {});
  void Stop();

 private:
  void* process_ = nullptr;
  void* readEnd_ = nullptr;
};

}  // namespace sb::proc
