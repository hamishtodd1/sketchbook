// A viz window, assembled into the shaders that run it.
//
// A viz is one box: `Viz(Tri, "Space");` on its first line, and then GLSL
// functions with standard names --
//
//   `mesh`      lays the geometry down, with `emit(p, n, uv)`: run once, when the
//               window is built, and again whenever it is edited
//   `vert`      the vertex shader: where each vertex goes, given the value
//   `frag`      the fragment shader; optional, and without it the dw lights it
//   `position`  where the viz is in its dw, which is where wires attach
//   `grab`      how far the pointer is from taking hold of it, in pixels
//   `drag`      the value the pointer is now asking for
//
// A viz of more than one piece numbers the rest: `mesh2`, `vert2`, `frag2`, up
// to 4. Everything else in the window is a helper, and every stage gets those.
//
// One box, three programs: a compute program running `mesh` and the three
// questions, and a vertex and fragment shader for the drawing. Each gets only
// the standard functions that are its own -- a `frag` using `fwidth` or
// `discard` would not compile anywhere but a fragment shader -- and the rest of
// their lines are left blank, so a line number is the same line in all three.
//
// Like the rest of the toolchain, this knows nothing about raylib.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "types/Clifford.h"

namespace sb::vizglsl {

inline constexpr int kMaxPieces = 4;
// How many vertices one piece's mesh may lay down, and how many numbers each
// one is: a position, a normal and a texture coordinate.
inline constexpr int kMeshCap = 16384;
inline constexpr int kVertexFloats = 8;
// Where a value's own numbers start in the run the dw hands over: its colour
// comes first.
inline constexpr int kTintFloats = 4;
// What the compute program writes back per value: its position, then how far
// the pointer is from it.
inline constexpr int kAnswerFloats = 4;

// The windows that ask for vizes. A viz names one of them.
bool KnownWindow(const std::string& dw);
// All of them, as a sentence, for a message.
std::string KnownWindows();

// --- a viz of a function ------------------------------------------------------
//
// A function is named by what goes in and what comes out -- `RToR`, `R2ToR3`,
// `CToC`, `R3ToR` -- with R, R2, R3 and C for float, vec2, vec3 and Complex. A
// viz of one is drawn in its fragments: `vec4 frag(vec2 at)` and optionally
// `float grab(vec2 at)`, where `value(...)` is the function being drawn. It is
// compiled into every box that defines a function of that type, renamed so
// that several can live in one box.

// The GLSL types either side of a function type's arrow, if it is one.
bool FunctionType(const std::string& type, std::string* in, std::string* out);
// The same, from a function's own two types.
std::string FunctionTypeOf(const std::string& in, const std::string& out);
// The windows a function type is drawn in, and what `sbShape` each is in a box's
// shader. Empty for a type no window draws.
std::vector<std::string> FunctionWindows(const std::string& type);
int                      ShapeOf(const std::string& dw);
// What a viz's own name becomes once it is compiled into a box: `frag` of an
// `RToR` on a `Plot` is `sbFrag_RToR_Plot`, and `value` is `sbValue_RToR_Plot`.
std::string Spliced(const std::string& name, const std::string& type, const std::string& dw);
// Where a fragment of that window is in its own space, as a box's shader works
// it out: the value along an axis, a point of a plot or of the plane, or the
// film of a camera.
std::string AtOf(const std::string& dw);
// Everything the app gives a function's viz: how colours are painted, where the
// camera is, and the march a surface and a volume are drawn with. The surface
// march calls the surface's `value`, so this comes after that is defined.
std::string FunctionHelpers(bool paintReal, bool paintColour, bool paintComplex, bool surface,
                            bool volume);
// The uniforms a box's shader gives a drawing, declared the same way here.
std::string FunctionUniforms();

struct Sources {
  std::string compute;
  std::string vertex;
  std::string fragment;
  // The same three with only the structs of the algebra's types, for a viz that
  // only reads a value's parts: tried first, since it compiles in a fraction of
  // the time. Empty, with `lean` false, where there is no algebra to leave out.
  bool        lean = false;
  std::string leanCompute;
  std::string leanVertex;
  std::string leanFragment;
  // The line of each source that the box's first line lands on. The same in
  // all three, so one map reads a diagnostic from any of them. What the sketch
  // shares comes just before it, after line `sharedAt`.
  int bodyAt = 0;
  int sharedAt = 0;
  // How many pieces, and which have what.
  int  pieces = 0;
  bool frag[kMaxPieces] = {};
  // A piece whose mesh is a model read from a file rather than laid down by its
  // own GLSL -- `void mesh2() { model("hands.obj"); }` -- names the file here.
  // Its function is not compiled at all.
  std::string model[kMaxPieces];
  bool position = false;
  bool grab = false;
  bool drag = false;
  // `T example()`: how a new value of the type is chosen.
  bool example = false;
  // How many numbers one value is.
  int stride = 1;

  // A viz of a function: its text as it is compiled into a box -- renamed, and
  // with its operators rewritten -- and the same standing alone against a
  // stand-in function, which is what is compiled to find its mistakes.
  bool        function = false;
  std::string spliced;
  std::string standalone;

  // What is wrong with the window, if anything, and on which line (0-based).
  std::string error;
  int         errorLine = 0;
};

// `lines` is the whole box, sigil and all. `algebraTypes` are the algebra's
// types the box mentions, which have to be written out for it. `imports` is
// what it takes from its sketch, and `seed` gives the shared values their
// values at the top of every `main`.
Sources Assemble(const std::vector<std::string>& lines, const std::string& type,
                 const std::string& dw, const clifford::Signature& sig,
                 const std::vector<std::string>& algebraTypes,
                 const std::string& imports = std::string(),
                 const std::string& seed = std::string());
// A viz of a function: `imports` is what it takes from its sketch (functions'
// declarations and text), `renames` what those names and the shared values become
// once spliced, `uniforms` the knobs it reads, declared for its standalone check
// -- the box it is spliced into declares them itself, once.
Sources AssembleFunctionViz(const std::vector<std::string>& lines, const std::string& type,
                            const std::string& dw, const std::string& imports,
                            const std::map<std::string, std::string>& renames,
                            const std::string& uniforms);
// The random numbers an `example` of a function uses, from a state set to the
// name of the function it is choosing: in every box's shader, before its
// functions, since a chosen body sets it.
std::string ExampleRandom();

// The names of the functions a viz box defines -- its own, never taken from
// the sketch.
std::vector<std::string> FunctionNames(const std::vector<std::string>& lines);
// Every function a box defines, whatever it takes: first and last line.
std::vector<std::pair<int, int>> FunctionSpans(const std::vector<std::string>& lines);
// `mesh`, `vert2`, `grab`...: a name a viz gives a meaning to.
bool IsStandardName(const std::string& name);
// What only a viz has -- `emit`, the pointer, the camera, `sbUncut` -- as
// stand-ins that do nothing, for a viz box's own shader: there it is built as
// any box is, for its windows, and nothing of a viz's is real.
std::string Stubs();
// The lines of a mesh that is a model file, `void mesh2() { model("x.obj"); }`:
// not GLSL, so nothing but a viz's own build may see them.
std::vector<bool> ModelLines(const std::vector<std::string>& lines);

}  // namespace sb::vizglsl
