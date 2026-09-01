// Monokai syntax colouring for one line of C++.
//
// Line-at-a-time, because that is how the editor stores and draws text. The
// consequence is that a block comment or a string running across a newline is
// not tracked; for boxes of a few lines of mathematics that has not mattered.
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "raylib.h"

namespace sb::highlight {

struct Span {
  std::size_t begin = 0;  // byte offsets into the line
  std::size_t end = 0;
  Color       color{};
};

// Covers the whole line, in order, with no gaps.
std::vector<Span> Cpp(std::string_view line);

}  // namespace sb::highlight
