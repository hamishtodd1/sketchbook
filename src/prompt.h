// What Claude is told when it is asked something on the canvas: what this
// program is, how its language reads, how its words will be set -- and the tools
// it has to look at the canvas and change it.
//
// One prompt for everything, deciding for itself when a question wants an
// answer, a box, or a plan and a question back first. **It is a file in the
// repo, `prompt/system.md`**, read with every message: text to be read and
// changed like the vizes sketches, not a string buried in the code. The tools
// stay here, since their shapes are what the code answers to. Raylib-free.
#pragma once

#include <string>

#include "json.h"

namespace sb::prompt {

const std::string& System();
// The tools, as the Messages API takes them.
const json::Value& Tools();

}  // namespace sb::prompt
