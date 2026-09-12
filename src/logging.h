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
// What survives is a whitelist -- this app's own six prefixes -- rather than a
// list of things to hide. raylib has dozens of subsystems and they all talk;
// this app has a handful and each only speaks when something has gone wrong. A
// new prefix has to be added here to be heard, which is a small price for a log
// that is worth reading, and `--verbose` turns the whole filter off.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "raylib.h"

namespace sb::log {

inline bool g_verbose = false;

// Said by this app rather than by the library under it.
inline bool Ours(const char* text) {
  // `GL:` is deliberately absent: that prefix is raylib's, and this app's own
  // entry-point notices say `GLEXT:` so the two can be told apart.
  static const char* const kPrefixes[] = {"CANVAS:", "MEDIA:", "BLOCKS:",
                                          "GLEXT:", "SPACE:", "SHADER BOX:"};
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

}  // namespace sb::log
