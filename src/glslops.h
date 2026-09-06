// Operator overloading for GLSL, which does not have any.
//
// GLSL has *function* overloading -- `gp(Bi, Bi)` and `gp(Tri, Tri)` are two
// functions of one name, resolved by argument type -- but it has no operator
// overloading at all. So `a * b` cannot be a geometric product there, and that
// was the whole reason a box's text had to be C++ before it could be a shader.
//
// The way round it is a rewrite, and because GLSL resolves overloads itself the
// rewrite needs no types: it is purely syntactic.
//
//     a * b   ->  gp(a, b)          ~a  ->  rev(a)
//     a | b   ->  ip(a, b)          !a  ->  dual(a)
//     a ^ b   ->  op(a, b)          -a  ->  neg(a)
//     a & b   ->  rp(a, b)
//     a + b   ->  add(a, b)
//     a - b   ->  sub(a, b)
//     a / b   ->  div(a, b)
//
// Two consequences worth stating, because both are deliberate and both are
// visible to whoever writes a box:
//
// **The operators always mean the algebra.** `!` is the dual and never boolean
// negation; `~ ^ & |` are never bitwise. Deciding otherwise would mean knowing
// the type of every expression, which is a type checker; declaring it instead
// costs one rule and no machinery. Boolean work uses `&&`, `||` and `not()`.
//
// **The precedence is the mathematician's, not C's.** Every algebra operator
// binds tighter than `+` and `-`, so `a + b | c` is `a + (b | c)` -- which is
// what it reads as. C++ binds `|` looser than `+` and the old rule here was
// "write the brackets", but that rule only existed because a C++ compiler was
// going to be the thing that ran it, and now none is.
#pragma once

#include <string>

namespace sb::glslops {

// Rewrites every algebra operator in `source` into the call GLSL can resolve.
// Line breaks are preserved exactly -- a diagnostic still points at the line
// the user wrote -- so the result has the same number of lines as the input.
// Returns false and fills `error` if the text will not tokenise.
bool Rewrite(const std::string& source, std::string* out, std::string* error);

// The same operations under the same names for the types GLSL already has, so
// that the rewrite above is safe on ordinary scalar, vector and matrix code and
// nothing anywhere has to know what type anything is. Emitted into every shader
// ahead of the user's lines.
std::string BuiltinOverloads();

}  // namespace sb::glslops
