// What the log says, and mostly what it does not.
//
// raylib narrates everything it does -- every texture, framebuffer and vertex
// array, loaded and unloaded, one line each -- and on a canvas that makes and
// throws away windows all day that is pages of it with nothing in them. So the
// log is **filtered rather than turned down**.
//
// Turned down would not work: raylib tests the level *before* it reaches the
// callback, and that callback is the only way a box's shader errors are caught
// at all (`CompileShaderHalf`). Lowering the level would take the diagnostics
// with the noise.
//
// What survives is a whitelist -- this app's own seven prefixes -- rather than a
// list of things to hide. raylib has dozens of subsystems and they all talk;
// this app has a handful and each only speaks when something has gone wrong. A
// new prefix has to be added here to be heard, which is a small price for a log
// that is worth reading, and `--verbose` turns the whole filter off.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "raylib.h"

namespace sb::log {

inline bool g_verbose = false;

// Said by this app rather than by the library under it.
inline bool Ours(const char* text) {
  // `GL:` is deliberately absent: that prefix is raylib's, and this app's own
  // entry-point notices say `GLEXT:` so the two can be told apart.
  static const char* const kPrefixes[] = {"CANVAS:", "MEDIA:", "BLOCKS:",
                                          "GLEXT:", "SPACE:", "SHADER BOX:", "VIZ:"};
  for (const char* prefix : kPrefixes) {
    if (std::strncmp(text, prefix, std::strlen(prefix)) == 0) return true;
  }
  return false;
}

inline void Filtered(int level, const char* text, va_list args) {
  if (!g_verbose && level < LOG_ERROR && !Ours(text)) return;
  const char* label = "INFO";
  if (level == LOG_WARNING) label = "WARNING";
  else if (level == LOG_ERROR) label = "ERROR";
  else if (level == LOG_FATAL) label = "FATAL";
  else if (level == LOG_DEBUG) label = "DEBUG";
  std::fprintf(stderr, "%s: ", label);
  std::vfprintf(stderr, text, args);
  std::fputc('\n', stderr);
}

inline void Install(bool verbose) {
  g_verbose = verbose;
  SetTraceLogCallback(Filtered);
}

// raylib reports a shader's compile errors through TraceLog and nowhere else, so
// for as long as one of these lives, what it says at warning or above is caught
// into `into` instead. Not re-entrant, and need not be: shaders are compiled one
// at a time, on the thread that draws.
inline std::string* g_caught = nullptr;

inline void Catching(int level, const char* text, va_list args) {
  if (g_caught == nullptr || level < LOG_WARNING) return;
  char buffer[2048];
  std::vsnprintf(buffer, sizeof(buffer), text, args);
  *g_caught += buffer;
  *g_caught += '\n';
}

struct Catch {
  explicit Catch(std::string* into) {
    g_caught = into;
    SetTraceLogCallback(Catching);
  }
  // Back to the filter rather than to nothing: putting nothing back would let
  // raylib narrate freely again from the next texture onwards.
  ~Catch() {
    SetTraceLogCallback(Filtered);
    g_caught = nullptr;
  }
  Catch(const Catch&) = delete;
  Catch& operator=(const Catch&) = delete;
};

}  // namespace sb::log
