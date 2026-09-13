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
//   p.x                   ->  p_{x}
//   vec3(a, b, c)         ->  (a, b, c)
//   Complex(a, b)         ->  a + bi
//
// A function is read as the arrow it is, and what it comes to as the equality
// that settles it:
//
//   vec3 swirl(vec2 p) {  ->  swirl : \mathbb{R}^{2} \to \mathbb{R}^{3} \;\{
//     ...                     ... each line reading for itself ...
//     return <expr>;      ->  swirl(p) := <expr>
//   }                     ->  \}
//
// Those last two are the only lines here that cannot be read on their own: a
// `return` is not a statement about anything until you know which function it
// is the answer of. So the caller passes an `Inside`, worked out from the spans
// FindFunctions reports. Everything else in this file still reads one line and
// asks nothing about its neighbours.
//
// Expressions are numbers, identifiers, calls, member access, + - * / and
// parentheses. A lambda is read only as a whole initialiser, which is where one
// can appear.
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
  Algebra,      // `Cl(3,0,1);` -- which algebra the box works in
  Viz,          // `Viz(Real);` -- this box draws that type, rather than using it
  Samples,      // `Samples(float, 24);` -- this box spreads that type out
  Signature,    // `vec3 swirl(vec2 p) {` -- an arrow between two spaces
  Prototype,    // `vec3 swirl(vec2 p);` -- the same arrow, with no body given
  Return,       // `return <expr>;` -- what the function it is in comes to
  Closing,      // the `}` that ends one: punctuation, and reads as nothing
};

struct Line {
  Kind        kind = Kind::Blank;
  std::string type;            // "float" / "double", for declarations
  std::string name;            // the variable declared or assigned; the type a Viz draws
  bool        hasInitialiser = false;
  int         count = 0;       // Samples: how many of them
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

// What a line is inside, when that is what decides whether it means anything.
//
// Most lines stand alone, and this file has always read them one at a time. Two
// do not: a `return` is not a statement about anything until you know which
// function it is the answer of, and the `}` after it is punctuation for the
// same reason. Only the caller can see the lines around a line, so only the
// caller can say.
struct Inside {
  std::string function;    // empty when the line is out in the open
  std::string parameter;
  // Whether this is the line the function *ends* on. A loop inside a function
  // closes with a brace too, and that one is still holding a block open -- only
  // the last one has had everything above it already said.
  bool closes = false;

  bool operator==(const Inside& other) const {
    return function == other.function && parameter == other.parameter &&
           closes == other.closes;
  }
};

// Never throws and never reports an error: anything unrecognised is Unparsed.
//
// The signature says which algebra a box is working in, which decides both what
// counts as a type name and how one is written down. It only matters for a box
// that uses geometric algebra; everything else ignores it.
Line Parse(std::string_view source,
           const clifford::Signature& sig = clifford::kDefaultSignature,
           const Inside& inside = {});

// Renders one identifier the way the line renderer would, so callers can label
// a number line with the same glyphs the equation uses.
std::string NameToLatex(std::string_view name);

// --------------------------------------------------------------- functions
//
// A function is recognised by its **shape**, not by a type name: `Tri p(float t)`
// is a function exactly as `float f(float x)` is. That matters because a
// function is not a value here -- it is a machine, and the only things a window
// needs to know about one are what goes in and what comes out.
struct FunctionHead {
  std::string returnType;
  std::string name;
  std::string paramType;
  std::string paramName;
  // Whether a body follows. `float f(vec3 q) {` opens one; `float f(vec3 q);`
  // is a **prototype** -- the user saying what a function *is* without saying
  // what it does, which is most of what one wants to do with a general
  // function. The app writes the body for it, and hovering shows what it wrote.
  bool        body = true;
};

// True if this line declares a function: either opening a definition, or a
// prototype ending in a semicolon. `out->body` says which. A definition's body
// may run on for as many lines as it likes; where it ends is a matter of
// counting braces, which is done by FindFunctions just below -- this function
// reads one line, which is what everything else in this file does too.
bool ParseFunctionHead(std::string_view source, FunctionHead* out,
                       const clifford::Signature& sig = clifford::kDefaultSignature);

// Where each function in a run of lines begins and ends.
//
// A function is recognised by its shape -- a known type, a name, one typed
// parameter, an opening brace -- and it runs until the braces balance again.
// That span is what makes "this line is inside that function" a question with
// an answer, and it is asked from two directions: which function a window
// shows, and what a `return` is the answer of.
struct FunctionSpan {
  FunctionHead head;
  int          first = 0;
  int          last = 0;
};

std::vector<FunctionSpan> FindFunctions(const std::vector<std::string>& lines,
                                        const clifford::Signature& sig =
                                            clifford::kDefaultSignature);

// Which of them holds this line, or -1 for a line out in the open.
int SpanAt(const std::vector<FunctionSpan>& spans, int line);

// What a type is called in the shader. `RToR` is the one name that is
// ours rather than GLSL's: it says *which* arrow a function is, where GLSL only
// knows the thing returns a float.
std::string GlslTypeName(std::string_view type);

// The name a declaration of this type gives its variable, or empty if the type
// is not one we know. `float` and `double` are reals.
bool IsRealType(std::string_view type);
// The plane and the space of ordinary GLSL, and the plane with a multiplication
// on it. They are types a function can take and give, which is the only reason
// this file has to know them: a function is recognised by its shape.
bool IsVectorType(std::string_view type);
bool IsComplexType(std::string_view type);
// The plane and the space of ordinary GLSL, and the plane with a multiplication
// on it. They are types a function can take and give, which is the only reason
// this file has to know them: a function is recognised by its shape.
bool IsVectorType(std::string_view type);
bool IsComplexType(std::string_view type);
// An element of a Clifford algebra: Bi, Even, ScalarQuad, ...
bool IsMultivectorType(std::string_view type, const clifford::Signature& sig);

}  // namespace sb::mathline
