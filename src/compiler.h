// Diagnostics from the driver.
//
// There is one language now: a box is GLSL, compiled by the graphics driver on
// the thread that owns the context. This file is what is left of the toolchain
// once the C++ half went -- the shape of a diagnostic, and how to read one out
// of a driver's log.
#pragma once

#include <string>

namespace sb::compiler {

// Where the earliest diagnostic points, in the block's own coordinates.
struct Diagnostic {
  bool        present = false;
  int         line = 1;    // 1-based
  int         column = 1;  // 1-based, in bytes
  std::string message;     // just the text, with the file/line prefix stripped
  // The next error after that one, if there was one, said with its own line.
  // One mistake usually has another right behind it -- a missing semicolon
  // reports twice, a misspelled name reports wherever it is used -- and seeing
  // the second saves a round trip through the compiler to find it.
  std::string secondary;
};

// Where the box's lines ended up in the assembled shader. A box's text appears
// *twice*: once as the functions hoisted above main -- GLSL has nowhere else to
// put a function -- and once as the body inside it. So mapping a driver's line
// back to the line that was written needs both starts, not one offset.
struct ShaderLines {
  int functionsAt = 0;   // first line of the hoisted block
  int bodyAt = 0;        // first line of main's body
  int count = 0;         // how many lines the box has
};

// Every vendor words its log differently. The two shapes in the wild are
// `0(12) : error ...` and `ERROR: 0:12: ...`, and both are read here.
Diagnostic ParseGlslLog(const std::string& log, const ShaderLines& where);

}  // namespace sb::compiler
