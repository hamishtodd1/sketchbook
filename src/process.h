// Running a child process and reading what it prints as it goes.
//
// Like touch_win32, this deliberately never includes raylib.h, because
// <windows.h> also declares Rectangle, CloseWindow and ShowCursor.
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace sb::proc {

// Quotes a path for use as one argument of a command line.
std::string Quote(const std::string& value);

// Runs a command to completion and returns everything it wrote to stdout, or
// nothing if it could not be started. For a tool that says a little and stops.
std::string Capture(const std::string& commandLine);

// A child whose output is read *while it runs* rather than collected at the
// end. That is the shape a decoder wants: it says a great deal, and would
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

// The other way round: a child that reads what it is given, as an encoder reads
// frames. What it says goes to `logPath`, so a failure can be read afterwards.
class Feed {
 public:
  Feed() = default;
  ~Feed();
  Feed(const Feed&) = delete;
  Feed& operator=(const Feed&) = delete;

  bool Start(const std::string& commandLine, const std::string& logPath);
  // Writes all of it, waiting while the child catches up. False once the child
  // has stopped reading.
  bool Write(const unsigned char* data, std::size_t bytes);
  // Says there is no more, and waits for the child to finish with it. Its exit
  // code, or -1 if it would not stop.
  int  Finish();
  // Stops it without waiting for it to finish: the work is being thrown away.
  void Stop();
  bool Running() const { return process_ != nullptr; }

 private:
  void* process_ = nullptr;
  void* writeEnd_ = nullptr;
};

}  // namespace sb::proc
