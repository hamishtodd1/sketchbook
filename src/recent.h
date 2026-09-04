// The canvases opened lately, kept between runs.
//
// Raylib-free, like the rest of the layer that talks to the outside world: this
// is a file on disk and a list of strings, and nothing in here knows what a
// window is.
#pragma once

#include <string>
#include <vector>

namespace sb::recent {

// How many are kept. A menu is a way back to what you were just working on, not
// a history, so the list is short enough to read at a glance.
inline constexpr int kMaxRecent = 8;

// Most recent first. A file that is no longer there is left out rather than
// offered and then failing: a place you cannot go back to is not a place.
std::vector<std::string> List();

// Put one at the front, moving it there if it is already in the list. Written
// out at once rather than at exit, so a canvas opened is remembered even if the
// app never closes tidily.
void Remember(const std::string& path);

}  // namespace sb::recent
