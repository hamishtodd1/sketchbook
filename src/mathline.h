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
  Viz,          // `Viz(Real);` -- this box draws that type, rather than using it
};

struct Line {
  Kind        kind = Kind::Blank;
  std::string type;            // "float" / "double", for declarations
  std::string name;            // the variable declared or assigned; the type a Viz draws
  bool        hasInitialiser = false;
  std::string latex;           // empty unless there is a reading

  // Every identifier written in `latex` is wrapped in \sbref{k}{...}, and
  // marks[k - 1] is the name it mentions. The tag survives typesetting, so the
  // renderer can find where on screen a given variable was written and draw a
  // box round it. That is how a window and the name it stands for are shown to
  // be the same thing.
  std::vector<std::string> marks;
};

// --------------------------------------------------------------- wiring
//
// The shape of what a line computes, as a little graph: leaves that name
// variables, and operator nodes fed by other nodes. `d = a + b * c` is a `*` fed
// by b and c, feeding a `+` fed by a, feeding d.
//
// Deliberately *not* the parser's own tree. Nothing outside needs to know about
// precedence, or about the difference between a call and a product; a wiring
// diagram needs only what feeds what, and what to write in the little box where
// two things meet. Numbers are not nodes at all -- a literal has nothing to come
// from, so the operator it feeds simply has one input.
struct FlowNode {
  std::string op;         // "+", "*", "|", ... empty for a leaf
  std::string name;       // a leaf naming a variable; empty for an operator
  int         lhs = -1;   // which node feeds it, or -1 for nothing
  int         rhs = -1;
};

struct Flow {
  std::string           result;   // the variable this line defines
  std::vector<FlowNode> nodes;    // the root is the last
  int                   root = -1;

  // Worth drawing: it computes something from something, rather than being a
  // bare value or a copy.
  bool Interesting() const {
    if (root < 0 || nodes.empty()) return false;
    for (const FlowNode& node : nodes) {
      if (!node.op.empty()) return true;
    }
    return false;
  }
};

// The wiring of one line, or an empty Flow if it has none.
Flow Wiring(std::string_view source,
            const clifford::Signature& sig = clifford::kDefaultSignature);

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
