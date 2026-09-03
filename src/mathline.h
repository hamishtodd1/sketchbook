// One line of the mathematical subset of C++.
//
// This is the bridge between the two halves of the program. It reads a single
// line of real C++ and, if that line has a mathematical reading, reports what
// it declares and what it looks like as LaTeX. Lines it does not understand are
// reported as such and stay code on screen -- that is the designed fallback,
// not a failure.
//
// The subset, deliberately tiny for now:
//
//   float a;              ->  a \in \mathbb{R}
//   float c = a + b;      ->  c = a + b
//   c = (a + b) / 2;      ->  c = \frac{a + b}{2}
//
//   RealFunction f;                          ->  f : \mathbb{R} \to \mathbb{R}
//   RealFunction h = f + 2 * g;              ->  h = f + 2g
//   RealFunction f = [](float x) {           ->  f(x) = \sin(1.4x)
//       return sin(1.4f * x); };
//
// Expressions are numbers, identifiers, calls, + - * / and parentheses. A
// lambda is read only as a whole initialiser, which is where one can appear.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "types/Clifford.h"

namespace sb::mathline {

enum class Kind {
  Blank,
  Unparsed,     // valid C++ perhaps, but no mathematical reading
  Declaration,  // `float a;` or `float c = <expr>;`
  Assignment,   // `c = <expr>;`
  Algebra,      // `Algebra(3,0,1);` -- which algebra the box works in
};

struct Line {
  Kind        kind = Kind::Blank;
  std::string type;            // "float" / "double", for declarations
  std::string name;            // the variable declared or assigned
  bool        hasInitialiser = false;
  std::string latex;           // empty unless there is a reading

  // Every identifier written in `latex` is wrapped in \sbref{k}{...}, and
  // marks[k - 1] is the name it mentions. The tag survives typesetting, so the
  // renderer can find where on screen a given variable was written and draw a
  // box round it. That is how a window and the name it stands for are shown to
  // be the same thing.
  std::vector<std::string> marks;
};

// Never throws and never reports an error: anything unrecognised is Unparsed.
//
// The signature says which algebra a box is working in, which decides both what
// counts as a type name and how one is written down. It only matters for a box
// that uses geometric algebra; everything else ignores it.
Line Parse(std::string_view source,
           const clifford::Signature& sig = clifford::kDefaultSignature);

// Renders one identifier the way the line renderer would, so callers can label
// a number line with the same glyphs the equation uses.
std::string NameToLatex(std::string_view name);

// The name a declaration of this type gives its variable, or empty if the type
// is not one we know. `float` and `double` are reals; `RealFunction` is a
// function from reals to reals.
bool IsRealType(std::string_view type);
bool IsFunctionType(std::string_view type);
// An element of a Clifford algebra: Bi, Even, ScalarQuad, ...
bool IsMultivectorType(std::string_view type, const clifford::Signature& sig);

}  // namespace sb::mathline
