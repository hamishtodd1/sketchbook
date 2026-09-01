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

#include "compiler.h"
#include "types/RealFunction.h"

namespace sb::mathprogram {

// What a variable is. Each kind needs two things of its type: how to render it
// as LaTeX, and how to make an example of it.
enum class Kind {
  Real,      // float / double
  Function,  // RealFunction: a function from reals to reals
};

struct Variable {
  std::string name;
  std::string latexName;
  Kind        kind = Kind::Real;
  bool        free = false;    // declared with no initialiser: yours to choose
  float       example = 0.0f;  // what the type's example generator produced
  float       value = 0.0f;    // Real: what it currently is
  bool        known = false;   // has a value actually been computed yet
  // Function: what it evaluates to across the sample window below. A function
  // cannot be printed as a number, so this is how one gets back to the screen.
  std::vector<float> samples;
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
  void Rebuild(const std::vector<std::string>& lines);

  // Collects finished builds and runs. Call every frame.
  void Poll();

  // Sets a free variable and schedules a re-run. Repeated calls while a run is
  // in flight coalesce into a single follow-up, so dragging stays smooth.
  void SetValue(std::size_t index, float value);

  // Values to adopt on the next rebuild, for a canvas loaded from disk. They
  // lose to anything already on screen, and are consumed once.
  void Restore(std::vector<std::pair<std::string, float>> values);

  // --- the shader half ----------------------------------------------------
  //
  // Compiling GLSL means talking to the graphics context, which this half of
  // the program knows nothing about. So it assembles the source and hands it
  // over: the host compiles it on the thread that owns the context and says
  // what happened.
  const std::string& glslSource() const { return glsl_; }
  // A second shader, the same lines over again, whose job is to hand a value
  // back rather than to draw. Each pixel of a one-row target reports one
  // derived variable, packed into its four bytes.
  const std::string&              glslProbeSource() const { return glslProbe_; }
  const std::vector<std::string>& probedNames() const { return probed_; }
  // What the probe read, in probedNames() order.
  void                            SetProbedValues(const std::vector<float>& values);
  // Lines of boilerplate before the user's first line in it, so a diagnostic
  // can be moved back into the box's own coordinates.
  int                shaderLineOffset() const { return glslOffset_; }
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

  // The shader the box would be, assembled alongside the C++ and only used if
  // the C++ is refused.
  std::string              glsl_;
  std::string              glslProbe_;
  std::vector<std::string> probed_;
  int                      glslOffset_ = 0;
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
};

}  // namespace sb::mathprogram
