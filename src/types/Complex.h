// Complex: the plane, with a multiplication.
//
// C is its own type here rather than a `vec2`, and that is the whole point of
// the file. `vec2` is R2 -- a *place* -- and a function of a place is drawn one
// way; a function of a complex number is drawn another, because the answer has
// an argument as well as a size and the usual way to show that is hue. Two
// types, two readings, and nothing has to guess which was meant.
//
// It is written out for the shader only when a box mentions it, exactly as an
// algebra is. No raylib here, and none in any type.
#pragma once

#include <string>
#include <vector>

namespace sb::types::complexnum {

// How the type is spelled.
inline constexpr const char* kName = "Complex";

// Its parts, in the order the struct declares them -- which is the order a
// value of it travels in, the same as a multivector's components.
std::vector<std::string> MemberNames();

// The GLSL that makes the type exist: the struct, and the products under the
// same names the operator rewrite produces. `z * w` becomes `gp(z, w)` there,
// so the multiplication has to answer to that name and not to `*`.
//
// The conjugate is `rev` -- `~z` -- because that is what the reverse is in the
// one-generator algebra C is, and because writing it twice under two names
// would be two things to remember.
std::string GlslPrologue();

// A value the app chose, for a declaration that named no value. Deterministic
// in the name, like every other example generator here.
std::string ExampleValue(const std::string& name);

}  // namespace sb::types::complexnum
