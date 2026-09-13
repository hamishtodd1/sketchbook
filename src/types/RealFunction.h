// A function nobody wrote a body for.
//
// A great deal of what this app is for is saying things about functions in
// general -- `float u(vec3 q);` and then statements about u -- without ever
// saying which function u is. A program cannot be about functions in general,
// so, exactly as with `float a;`, the app chooses one. That choice is a body,
// and it is injected where the prototype stands, unless the box goes on to give
// the function a body of its own.
//
// No raylib here, and none in any type: the mathematics and the toolchain do
// not know that a renderer exists.
#pragma once

#include <string>
#include <string_view>

#include "types/Clifford.h"

namespace sb::types::realfunction {

// The type key a function of reals to reals is drawn under: what a plot and a
// field ask the viz store for. Nobody declares anything with it -- a function
// is declared by its signature -- but `Viz(RToR)` is how a viz box would name
// it.
inline constexpr const char* kName = "RToR";

// The expression the app returns from a function of `paramType` giving back
// `returnType`, written in terms of `paramName`.
//
// Deterministic in the function's name, so it keeps the same example between
// runs instead of jittering every time something is rebuilt. One generator
// behind what a hover shows and what the shader runs, so the two cannot drift
// apart.
std::string ExampleFunctionBody(std::string_view returnType, std::string_view paramType,
                                std::string_view paramName, std::string_view name,
                                const clifford::Signature& sig);

}  // namespace sb::types::realfunction
