// The built-in C++ compiler.
//
// A code block is treated as a translation unit that must define
// `bool testFunc()`. We append a `main` that returns 0 for true and 1 for
// false, build a real executable with the system C++ compiler, and run it out
// of process -- so a segfault or an infinite loop in the code on the canvas
// cannot take the app down with it.
//
// Nothing is prepended to the user's text, so a diagnostic at line N of the
// generated file is at line N of the block, and error positions map straight
// back to the caret.
#pragma once

#include <string>
#include <vector>

namespace sb::compiler {

enum class Status {
  NotRun,       // never compiled
  Working,      // a build is in flight
  Failed,       // the compiler rejected it
  True,         // testFunc() returned true
  False,        // testFunc() returned false
  Crashed,      // it built, but the process died or hung
  NoCompiler,   // no C++ compiler could be found
};

// Where the earliest diagnostic points, in the block's own coordinates.
struct Diagnostic {
  bool        present = false;
  int         line = 1;    // 1-based
  int         column = 1;  // 1-based, in bytes
  std::string message;     // just the text, with the file/line prefix stripped
};

struct Outcome {
  Status     status = Status::NotRun;
  Diagnostic error;
  std::string detail;  // populated for NoCompiler and Crashed
};

// Path of the C++ compiler this app will use, or empty if none was found.
// Probed once on first call.
const std::string& ToolchainPath();

// Builds and runs `source`. Blocking; call it on a worker thread.
Outcome BuildAndRun(const std::string& source);

// ---------------------------------------------------------------------------
// The two halves on their own, for callers that build once and then re-run the
// same executable with different inputs.

struct Build {
  bool        ok = false;
  Diagnostic  error;
  std::string exePath;   // caller owns it; pass to Discard when finished
  std::string detail;    // set when no compiler could be found
  bool        haveCompiler = true;
};

// `lineOffset` is how many lines the caller prepended to the user's own text;
// it is subtracted from diagnostics so they point back at the original.
Build Compile(const std::string& source, int lineOffset);

struct Run {
  bool        ok = false;
  bool        timedOut = false;
  int         exitCode = -1;
  std::string output;
};

Run Execute(const std::string& exePath, const std::vector<std::string>& arguments,
            int timeoutMs);

// Deletes a built executable.
void Discard(const std::string& exePath);

}  // namespace sb::compiler
