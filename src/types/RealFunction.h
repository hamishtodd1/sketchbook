// RealFunction: a function from reals to reals.
//
// There is almost nothing left of this type, and that is the point: a function
// is a GLSL function now, so `RealFunction` is only a *spelling* -- the one that
// says which arrow a function is, where GLSL knows only that the thing returns
// a float. What remains is the name, and the shape the app chooses when someone
// declares a function without saying what it is.
//
// No raylib here, and none in any type: the mathematics and the toolchain do
// not know that a renderer exists.
#pragma once

#include <string>

namespace sb::types::realfunction {

// How the type is spelled.
inline constexpr const char* kName = "RealFunction";

// The shape the app chose for a function nobody defined, as an expression in
// `x`. Deterministic in the name, so a variable keeps the same example between
// runs instead of jittering every time something is rebuilt.
std::string ExampleBody(const std::string& name);

// The same thing, as the definition that is compiled. One generator behind
// both, so what a hover shows and what the shader runs cannot drift apart.
std::string ExampleGlsl(const std::string& name);

}  // namespace sb::types::realfunction
