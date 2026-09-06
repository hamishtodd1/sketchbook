// The live program behind a box of mathematical lines.
//
// A box is compiled once. Its free variables are then passed in as command-line
// arguments and every variable's value is printed back, so moving a knob costs
// one process launch (a few milliseconds) instead of a rebuild.
//
// The declaration the user writes stays abstract -- `float a;`. The initialiser
// that makes it compile is injected here, never in their text.
#pragma once

#include <future>
#include <string>
#include <utility>
#include <vector>

#include <map>

#include "compiler.h"
#include "types/RealFunction.h"

namespace sb::mathprogram {

// What a variable is. Each kind needs two things of its type: how to render it
// as LaTeX, and how to make an example of it.
enum class Kind {
  Real,         // float / double
  Function,     // RealFunction: a function from reals to reals
  Multivector,  // an element of a Clifford algebra: Bi, Even, ...
};

// How a canvas says its own types are drawn: the C++ lines of every viz box on
// it, by the type it names. They are injected into every program that declares a
// value of that type, once per value, with `value` bound to it -- because the
// only place a function can be sampled is the program that made it.
using VizSources = std::map<std::string, std::vector<std::string>>;

// One literal assignment to a variable, and the uniform standing behind it.
//
// A variable is not a uniform -- it is an ordinary, mutable GLSL local, so it
// can be assigned over like any other. What is a uniform is each *literal* it
// is assigned: `float foo;` on line 2 (where the app injects one), `foo = 3.5;`
// on line 9, and so on, become `foo_2`, `foo_9`. The line then reads
// `float foo = foo_2;` and `foo = foo_9;`.
//
// That is what makes a knob work on a mutable variable. Dragging one edits the
// literal that *governs* the value being looked at -- the last one at or before
// the line the box is being read at -- because that is the number responsible
// for what is on screen. Read above line 9 and the knob moves `foo_2`; read
// below it and the same knob moves `foo_9`.
struct Literal {
  int         line = 0;
  std::string uniform;
  float       value = 0.0f;
};

struct Variable {
  std::string name;
  std::string type;       // as the user spelled it, which is what a viz claims
  std::string latexName;
  Kind        kind = Kind::Real;
  bool        free = false;    // declared with no initialiser: yours to choose
  // The line it is declared on. Before that line it does not exist, and a
  // window for it would be reporting a value it does not have yet.
  int         declaredLine = 0;
  float       example = 0.0f;  // what the type's example generator produced
  float       value = 0.0f;    // Real: what it currently is
  bool        known = false;   // has a value actually been computed yet
  // Function: what it evaluates to across the sample window below. A function
  // cannot be printed as a number, so this is how one gets back to the screen.
  std::vector<float> samples;
  // Multivector: its components, in the order its struct declares them. A
  // multivector is not one number, so it does not fit `value` -- but it is a
  // fixed, short list of them, so it needs nothing cleverer than this.
  std::vector<float> components;
  // Every literal assigned to this variable, in the order they are written.
  std::vector<Literal> literals;
  // Which of them decides the value at `line`: the last at or before it, or -1.
  int GoverningLiteral(int line) const {
    int best = -1;
    for (std::size_t i = 0; i < literals.size(); ++i) {
      if (line >= 0 && literals[i].line > line) break;
      best = static_cast<int>(i);
    }
    return best;
  }
  // What the canvas's viz for this type drew for this value: triangles and
  // lines, x and y alternating, in the viz's own units. Empty unless the canvas
  // defines one.
  std::vector<float> mesh;
  std::vector<float> lines;
};

// The point a fragment shader is being evaluated at. Worth a name out here
// because hovering a shader's picture *is* setting it: the pointer over the
// picture is that value, which makes it a variable like any other as far as the
// canvas is concerned.
inline constexpr const char* kShaderInput = "fragTexCoord";

// A box is C++ by default. If the C++ compiler rejects it, the same text is
// tried as GLSL -- the same lines, wrapped in different boilerplate -- because
// a box that says `vec2 p = fragTexCoord;` was never meant to be C++.
enum class State {
  Empty,        // nothing to build
  Building,     // a C++ compile is in flight
  Shading,      // C++ rejected it; whoever owns the GPU should try the shader
  Ready,        // built as C++, and values are live
  ShaderReady,  // built as a shader, and its uniforms are live
  Failed,       // neither language would take it
  NoCompiler,   // nothing to build it with
};

class Program {
 public:
  Program() = default;
  ~Program();
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;

  // Rebuilds from the box's lines. Values of variables that survive the edit
  // are carried over, so editing does not reset the knobs you have set.
  void Rebuild(const std::vector<std::string>& lines, const VizSources* vizes = nullptr);

  // Collects finished builds and runs. Call every frame.
  void Poll();

  // Sets a free variable and schedules a re-run. Repeated calls while a run is
  // in flight coalesce into a single follow-up, so dragging stays smooth.
  // Moves the literal governing `atLine`, which is the one responsible for what
  // the window showing this variable is displaying. -1 means the end.
  void SetValue(std::size_t index, float value, int atLine = -1);
  // The literal a uniform of this name stands behind, or null.
  const Literal* FindLiteral(const std::string& uniform) const;

  // Values to adopt on the next rebuild, for a canvas loaded from disk. They
  // lose to anything already on screen, and are consumed once.
  void Restore(std::vector<std::pair<std::string, float>> values);

  // --- a box that draws rather than computes -------------------------------
  //
  // `Viz(Real);` on a line of its own says this box is a **viz**: it says how a
  // value of that type is drawn inside a display window, rather than declaring
  // a value of its own. So it has no variables and grows no number lines.
  //
  // Two halves, one per box, paired by the type they name. The C++ half lays
  // down a **mesh** -- calls to `vertex(x, y)`, three to a triangle, in the
  // viz's own units -- and every vertex is printed back. The GLSL half is the
  // **vertex shader** that places those vertices, given the value and the scale
  // the window is drawn at. Which half a box is is decided the way it always
  // is: C++ first, and GLSL if C++ will not have it.
  const std::string&        vizType() const { return vizType_; }
  const std::vector<float>& vizMesh() const { return vizMesh_; }
  const std::vector<float>& vizLines() const { return vizLines_; }

  // --- the shader half ----------------------------------------------------
  //
  // Compiling GLSL means talking to the graphics context, which this half of
  // the program knows nothing about. So it assembles the source and hands it
  // over: the host compiles it on the thread that owns the context and says
  // what happened.
  const std::string& glslSource() const { return glsl_; }
  // The vertex half. Ours rather than raylib's default, because the fragment
  // half needs the texture coordinate under a private name: reporting a value
  // asks about one given point rather than the point being drawn, and a varying
  // cannot be shadowed by the local that chooses between them.
  const std::string& glslVertexSource() const;

  // --- a viz's mesh ---------------------------------------------------------
  //
  // The half of a viz box that lays geometry down is a *compute* shader now,
  // writing its vertices into a buffer the drawing then reads. It used to be
  // C++ injected into the program that owned the value, because a
  // `std::function` could only be sampled in the process that made it -- and
  // that reason went away when a function became GLSL.
  const std::string& glslMeshSource() const { return glslMesh_; }
  // How many vertices either half may lay down before it is cut off.
  static constexpr int kVizVertexCap = 4096;
  // In the order `sbIndex` picks them.
  const std::vector<std::string>& functionNames() const { return functionNames_; }

  // --- what every value was, after every line ------------------------------
  //
  // A variable is mutable, so "the value of a" is not a question until a *when*
  // is given. The shader writes every real's value after every line into one
  // buffer, so the when can be chosen afterwards -- by where the caret is --
  // without anything being run again.
  // One entry per number the trace carries: which variable it belongs to, and
  // which of its components -- -1 for a real, which is only ever one number.
  const std::vector<std::pair<std::string, int>>& tracedSlots() const { return tracedSlots_; }
  int                             tracedLines() const { return tracedLines_; }
  // `values` is tracedLines() * tracedNames().size() floats, line-major.
  void TakeTrace(std::vector<float> values);
  // Sets every real to what it was after `line`, which is what the windows then
  // show. Out of range means the end, which is where a box with no caret in it
  // is read from.
  void ApplyTrace(int line);
  const std::vector<std::string>& probedNames() const { return probed_; }
  // What the probe read, in probedNames() order.
  void                            SetProbedValues(const std::vector<float>& values);
  // Lines of boilerplate before the user's first line in it, so a diagnostic
  // can be moved back into the box's own coordinates.
  int                shaderLineOffset() const { return glslLines_.bodyAt; }
  void               ShaderCompiled(bool ok, const std::string& log);

  State                        state() const { return state_; }
  const std::vector<Variable>& variables() const { return variables_; }
  const compiler::Diagnostic&  error() const { return error_; }
  const std::string&           detail() const { return detail_; }
  // How many lines were prepended to the user's text, for diagnostic mapping.
  static int                   LineOffset();

 private:
  // One printed line: a name and the numbers after its `=`. A real prints one
  // number, a function prints its whole sample run.
  struct RunValue {
    std::string        name;
    std::vector<float> numbers;
  };
  struct RunOutput {
    bool                  ok = false;
    std::vector<RunValue> values;
  };

  void Launch();

  std::vector<Variable> variables_;
  State                 state_ = State::Empty;

  // The type this box draws, if it is a viz, and the mesh its C++ laid down --
  // x, y, x, y, three vertices to a triangle.
  std::string           vizType_;
  std::vector<float>    vizMesh_;
  std::vector<float>    vizLines_;

  // The shader the box would be, assembled alongside the C++ and only used if
  // the C++ is refused.
  std::string              glsl_;

  std::vector<std::string> functionNames_;
  std::string              glslMesh_;
  std::vector<std::pair<std::string, int>> tracedSlots_;
  int                      tracedLines_ = 0;
  std::vector<float>       trace_;
  std::vector<std::string> probed_;
  compiler::ShaderLines    glslLines_;
  // Whether the text mentions anything only GLSL has. It decides which of two
  // failures to report when neither language would take the box.
  bool                 looksLikeGlsl_ = false;
  compiler::Diagnostic cppError_;
  compiler::Diagnostic  error_;
  std::string           detail_;
  std::string           exePath_;

  std::vector<std::pair<std::string, float>> restore_;

  std::future<compiler::Build> build_;
  std::future<RunOutput>       run_;
  bool                         runQueued_ = false;
  // A rebuild asked for while one was already in flight. Dropping it would lose
  // the ask entirely -- which is how a box that had not finished its first
  // build would never learn that the canvas had said how to draw its type.
  bool                         rebuildQueued_ = false;
  std::vector<std::string>     queuedLines_;
  const VizSources*            queuedVizes_ = nullptr;
};

}  // namespace sb::mathprogram
