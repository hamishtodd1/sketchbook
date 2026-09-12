// RToR: a function from reals to reals.
//
// There is almost nothing left of this type, and that is the point: a function
// is a GLSL function now, so `RToR` is only a *spelling*. It earns its keep in
// exactly one place -- a declaration with no body, `RToR f;`, where there is no
// signature to read the arrow off and the app has to choose a function. Given a
// body, `RToR f(float x)` and `float f(float x)` are the same thing, because
// the shape says the arrow.
//
// No raylib here, and none in any type: the mathematics and the toolchain do
// not know that a renderer exists.
#pragma once

#include <string>

namespace sb::types::realfunction {

// How the type is spelled. It names the arrow, where `RealFunction` -- what it
// was called before -- only said which reals were involved.
inline constexpr const char* kName = "RToR";

// The shape the app chose for a function nobody defined, as an expression in
// `x`. Deterministic in the name, so a variable keeps the same example between
// runs instead of jittering every time something is rebuilt.
std::string ExampleBody(const std::string& name);

// The same thing, as the definition that is compiled. One generator behind
// both, so what a hover shows and what the shader runs cannot drift apart.
std::string ExampleGlsl(const std::string& name);

}  // namespace sb::types::realfunction
