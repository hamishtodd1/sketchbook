// The live program behind a box of mathematical lines.
//
// A box is one shader. Its free variables reach it as uniforms standing behind
// the literals they are assigned, so moving a knob costs the next frame rather
// than a rebuild.
//
// The declaration the user writes stays abstract -- `float a;`. The initialiser
// that makes it compile is injected here, never in their text.
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compiler.h"
#include "types/Clifford.h"
#include "types/RealFunction.h"
#include "vizglsl.h"

namespace sb::mathprogram {

// What a variable is. Each kind needs two things of its type: how to render it
// as LaTeX, and how to make an example of it.
enum class Kind {
  Real,         // float / double
  Function,     // RToR: a function from reals to reals
  Multivector,  // an element of a Clifford algebra: Bi, Even, ...
  Color,        // a vec3 that is a colour: three numbers, one knob
};

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
  // The number is typed in the text on `line`, rather than injected by the app.
  // Moving its knob then writes the number back, so the text and what runs
  // never disagree.
  bool        written = false;
  // A colour's three numbers, behind one vec3 uniform, or a multivector's, one
  // per member, behind one float array. Empty for a real, whose one number is
  // `value`.
  std::vector<float> components;
  // Behind a float array rather than a vec3: a multivector.
  bool        array = false;
};

// What a type is, *as a space*.
//
// A function is drawn by what goes in and what comes out, and those are the
// same question asked twice: how many dimensions, and what kind of thing. The
// answer decides everything about how one is shown --
//
//   **the input decides the window.** One dimension is a line of pixels: the
//   field along a number line. Two is a square of them: a picture. Three is a
//   volume. Nothing else about the function comes into it.
//
//   **the output decides the colour.** A real is viridis, signed. A vec3 *is* a
//   colour and is used as one. A complex number has an argument as well as a
//   size, so it gets hue for the one and lightness for the other.
//
// That is why `Complex` is not `vec2`. They are the same two numbers and they
// are not the same space: one is a place, the other is a place with a
// multiplication, and the second is worth drawing differently.
enum class Space {
  None,   // nothing this canvas knows how to draw
  R1,     // float
  R2,     // vec2
  R3,     // vec3
  C,      // Complex
  Multi,  // an element of the box's algebra
};

// How many numbers one value of it takes. A multivector's count depends on
// which one, so it is not answered here.
int Dimension(Space space);

// A function defined in the box: `Tri p(float t) { ... }`.
//
// A function is recognised by its **shape**, not by a type name -- it is not a
// value here, it is a machine, and the only things a window needs of one are
// what goes in and what comes out. It runs from its signature to the brace that
// closes it, and that span is what makes "the caret is inside this function" a
// question with an answer -- which is what decides which function is being
// shown.
struct Function {
  std::string name;
  std::string inputType;    // as the user spelled it
  std::string outputType;
  std::string parameter;       // the argument's name, `p` in `vec3 f(vec2 p)`
  int         firstLine = 0;   // the signature
  int         lastLine = 0;    // the brace that closes it
  // How many numbers one value takes: 1 for a real, one per component for a
  // multivector. Samples travel as a flat run of floats, so this is the stride.
  int         inStride = 1;
  int         outStride = 1;
  // What those types are as spaces, which is what decides how one is drawn.
  Space       in = Space::None;
  Space       out = Space::None;
  // A function of reals to reals is already drawn two ways -- a curve on a plot
  // and a field on an axis -- and those special cases win where they apply.
  // Everything else falls back on being sampled.
  bool        sampled = false;

  bool Spans(int line) const { return line >= firstLine && line <= lastLine; }

  // A function of two variables is a picture: one pixel per input. Whether the
  // plane it is over is R2 or C changes only how the answer is coloured.
  bool IsPicture() const {
    return (in == Space::R2 || in == Space::C) &&
           (out == Space::R1 || out == Space::R3 || out == Space::C);
  }
  // **And a real-valued one is a surface.** A colour has nowhere to go but the
  // pixel it is at; a *number* has a third direction available, and using it is
  // the difference between a picture of a landscape and the landscape. So this
  // one is drawn as relief, seen from straight above to begin with -- which is
  // the flat picture exactly -- and turned to be read as terrain.
  bool IsSurface() const { return in == Space::R2 && out == Space::R1; }
  // **And a function of three variables is a volume.** There is no surface to
  // stand on and no pixel to put the answer at, so it is drawn as what it is: a
  // quantity that is everywhere at once, thick where there is a lot of it and
  // clear where there is none.
  bool IsVolume() const {
    return in == Space::R3 && (out == Space::R1 || out == Space::C);
  }
  // Anything with a window of its own to be drawn in.
  bool IsDrawn() const { return IsPicture() || IsVolume(); }
};

struct Variable {
  std::string name;
  std::string type;       // as the user spelled it, which is what a viz claims
  std::string latexName;
  Kind        kind = Kind::Real;
  bool        free = false;    // declared with no initialiser: yours to choose
  // Declared inside one of the box's functions rather than at the top. It is a
  // local there, not one of the box's globals, and nothing traces it: the only
  // thing it can be is the number its knob chose.
  bool        local = false;
  // Declared with a number typed into it -- `float a = 0.4;` -- rather than with
  // no value for the app to inject. A knob like any free variable's; but its
  // number lives in the text, so the text is what is saved and what a closed
  // window leaves behind.
  bool        written = false;
  // The line it is declared on. Before that line it does not exist, and a
  // window for it would be reporting a value it does not have yet.
  int         declaredLine = 0;
  float       example = 0.0f;  // what the type's example generator produced
  float       value = 0.0f;    // Real: what it currently is
  bool        known = false;   // has a value actually been computed yet
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
};

class Program;

// --- what boxes share -------------------------------------------------------
//
// **Everything shareable is visible everywhere.** A function defined in any box
// can be called from any other, and a free value declared in any box can be read
// by any other -- the way a paper defines a thing once and then uses it. Nothing
// is imported by name and no box is special: the app gathers what every box
// offers into one `Canvas`, and a box that mentions a name it does not define
// itself gets that name compiled in.
//
// A function travels as text: its GLSL, with the knobs inside it as uniforms. A
// value travels as a uniform, so a knob in one box moves every box that reads it
// and nothing is compiled. A *derived* value does not travel at all -- it is
// made by running its box's lines, and sharing it would make the canvas one
// program with an order between its boxes, which a canvas does not have.
struct SharedFunction {
  std::string              name;
  std::string              declaration;   // `float f(float x);`
  std::string              text;          // its GLSL, operators already rewritten
  std::vector<std::string> uniforms;      // the knobs inside it
  const Program*           home = nullptr;
};

struct SharedValue {
  std::string    name;
  std::string    type;      // as GLSL spells it
  std::string    init;      // what it is set to where it is declared
  std::string    uniform;   // the knob behind it
  int            width = 1; // how many numbers that knob is: 3 for a colour
  bool           array = false;   // a float array, for a multivector
  const Program* home = nullptr;
};

// A viz of a function, as a box compiles it in: its text, renamed and
// rewritten, and whether it says how near the pointer is.
struct FunctionViz {
  std::string text;
  bool        grab = false;
  bool operator==(const FunctionViz& other) const {
    return text == other.text && grab == other.grab;
  }
};

// Everything the boxes of one canvas offer each other, and the one algebra they
// all work in. `home` is only ever compared, never followed, except by whoever
// gathered this -- a box can go away between one gathering and the next.
struct Canvas {
  clifford::Signature         signature = clifford::kDefaultSignature;
  // Two boxes naming different algebras. A canvas works in one, so this is an
  // error on every line that names one.
  bool                        algebraClash = false;
  std::vector<SharedFunction> functions;
  std::vector<SharedValue>    values;
  // Names defined in more than one box. With no order between boxes, "the later
  // one wins" means nothing, so each is an error in every box defining it.
  std::vector<std::string>    duplicated;
  // Which box each shared knob belongs to, so its number can be fetched.
  std::unordered_map<std::string, const Program*> uniformHome;
  // Names a box defines and does not share -- derived values, worked out by
  // running that box. Another box mentioning one gets told why it is missing,
  // rather than only that it is.
  std::vector<std::pair<std::string, const Program*>> unshared;
  // **Colour is handed out across the canvas**, not box by box: every name, in
  // the order the boxes are in and then the order each declares them. A name
  // means one thing on a canvas, so it gets one colour on it -- and two things
  // in one box can no longer both be the first colour of their own box.
  std::vector<std::string> colourOrder;
  // **How a function is drawn is a viz**, of its type in a window -- `RToR@Plot`
  // -- and a box that defines one compiles that viz in. The canvas's own, or
  // the built-in one.
  std::map<std::string, FunctionViz> functionVizes;
};

// Everything the boxes offer, gathered. Boxes that offer nothing -- words, a
// viz, a sampler -- are passed like any other and simply contribute nothing.
Canvas Gather(const std::vector<const Program*>& programs);

// There is one language. A box is GLSL, and compiling GLSL means talking to the
// graphics context -- which this half of the program knows nothing about and
// must keep knowing nothing about. So it assembles the source, stops, and waits
// to be told what the driver said.
enum class State {
  Empty,        // nothing to build
  Shading,      // assembled; whoever owns the GPU should compile it
  ShaderReady,  // built, and its uniforms are live
  Failed,       // the driver would not have it
};

class Program {
 public:
  Program();
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;

  // Rebuilds from the box's lines, in the algebra the canvas works in and with
  // whatever the canvas shares compiled in where the box mentions it. Values of
  // variables that survive the edit are carried over, so editing does not reset
  // the knobs you have set.
  void Rebuild(const std::vector<std::string>& lines, const Canvas& canvas);

  // --- sharing ---------------------------------------------------------------
  //
  // What this box offers the rest of the canvas. Worked out on every rebuild,
  // including one that fails, so a mistake in a box does not make its names
  // vanish and reappear -- which would make a clash come and go with it.
  const std::vector<SharedFunction>& sharedFunctions() const { return sharedFunctions_; }
  const std::vector<SharedValue>&    sharedValues() const { return sharedValues_; }
  // Every name the box defines at the top level, shared or not: a name means
  // one thing on a canvas.
  const std::vector<std::string>&    definedNames() const { return definedNames_; }
  // The algebra this box names with a `Cl(...)` line, if it names one.
  bool                               saysAlgebra() const { return saysAlgebra_; }
  const clifford::Signature&         saidAlgebra() const { return saidAlgebra_; }
  // Changes whenever what this box offers does, and never repeats between
  // boxes, so a list of these says whether anything on the canvas has changed.
  unsigned long                      sharedVersion() const { return sharedVersion_; }
  // Whether what this box took from `canvas` is no longer what the canvas says
  // -- in which case it has to be built again.
  bool                               ImportsStale(const Canvas& canvas) const;
  // Fetches the current numbers of the knobs this box reads from other boxes.
  // Called by whoever gathered `canvas`, while it is fresh.
  void                               PullImports(const Canvas& canvas);
  // The text this box has compiled in from elsewhere, for anything that needs
  // to ask what it mentions.
  const std::string&                 importedSource() const { return importedSource_; }
  // The names it took: shared values and functions defined in other boxes.
  const std::vector<std::string>&    importedNames() const { return importedNames_; }

  // Sets a free variable: moves the literal governing `atLine`, which is the one
  // responsible for what the window showing this variable is displaying. -1
  // means the end. Nothing is rebuilt; the uniform carries it.
  void SetValue(std::size_t index, float value, int atLine = -1);
  // The same for a colour, whose knob is three numbers.
  // A colour's or a multivector's numbers, as its knob now has them.
  void SetComponents(std::size_t index, const std::vector<float>& parts, int atLine = -1);
  // The literal a uniform of this name stands behind, or null -- this box's own,
  // or one it reads from another box.
  const Literal* FindLiteral(const std::string& uniform) const;
  // The knobs this box reads from other boxes.
  const std::vector<Literal>& imported() const { return imported_; }

  // Values to adopt on the next rebuild, for a canvas loaded from disk. They
  // lose to anything already on screen, and are consumed once.
  void Restore(std::vector<std::pair<std::string, float>> values);

  // --- a box that draws rather than computes -------------------------------
  //
  // `Viz(Tri, "Space");` on a line of its own says this box is a **viz**: how a
  // value of that type is drawn in that window, rather than a value of its own.
  // So it has no variables and grows no windows. Its lines are GLSL functions
  // with standard names, and `vizglsl.*` is what turns them into the three
  // programs that run it.
  //
  // `Tri@Space`: the type and the window, which is what a viz is keyed by.
  const std::string& vizType() const { return vizType_; }
  const vizglsl::Sources& viz() const { return viz_; }

  const std::string& glslSource() const { return glsl_; }
  // The vertex half. Ours rather than raylib's default, because the fragment
  // half needs the texture coordinate under a private name: reporting a value
  // asks about one given point rather than the point being drawn, and a varying
  // cannot be shadowed by the local that chooses between them.
  const std::string& glslVertexSource() const;

  // --- functions, and feeding them -----------------------------------------
  //
  // A function has no one place to be, so it is not shown as a value: it is
  // shown by being *fed*. A spread of samples of its input space is drawn in
  // whatever window that type goes in, every one of them is put through the
  // function, and the answers are drawn in whatever window the output type goes
  // in -- which may well be the same window.
  const std::vector<Function>& functions() const { return functions_; }
  // Which function is being read at `line`, or -1. Out of range means the end,
  // which for a box that is nothing but a definition is inside it.
  int FunctionAt(int line) const;

  // `Samples(float, 24);` makes a box a **sampler**: it says how a type's space
  // is spread out, and how thickly, rather than declaring a value of its own.
  // Its body lays samples down with `pick(...)`, exactly as a viz's `mesh` lays
  // vertices down with `emit(...)`, and for the same reason -- a spread is
  // geometry, and geometry belongs on the GPU.
  const std::string& samplerType() const { return samplerType_; }
  int                samplerCount() const { return samplerCount_; }
  int                samplerStride() const { return samplerStride_; }
  const std::string& glslSampleSource() const { return glslSample_; }
  // Anything that grows with use needs a bound, and this is a spread of dots on
  // a window: past a couple of hundred they are a smear rather than a set.
  static constexpr int kMaxSamples = 256;
  static constexpr int kMaxStride = 16;
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
  void                            ShaderCompiled(bool ok, const std::string& log);

  State                        state() const { return state_; }
  const std::vector<Variable>& variables() const { return variables_; }
  const compiler::Diagnostic&  error() const { return error_; }

 private:
  std::vector<Variable> variables_;
  State                 state_ = State::Empty;

  // The type this box draws, if it is a viz.
  std::string           vizType_;
  std::string           glsl_;

  std::vector<std::string> functionNames_;
  std::vector<Function>    functions_;
  std::string              samplerType_;
  int                      samplerCount_ = 0;
  int                      samplerStride_ = 1;
  std::string              glslSample_;
  vizglsl::Sources         viz_;
  std::vector<std::pair<std::string, int>> tracedSlots_;
  int                      tracedLines_ = 0;
  std::vector<float>       trace_;
  std::vector<std::string> probed_;
  compiler::ShaderLines    glslLines_;
  compiler::Diagnostic     error_;

  std::vector<std::pair<std::string, float>> restore_;

  // Unique to this box, so the uniforms of two boxes never share a name once
  // one box's functions are compiled into the other.
  int                         id_ = 0;
  std::vector<SharedFunction> sharedFunctions_;
  std::vector<SharedValue>    sharedValues_;
  std::vector<std::string>    definedNames_;
  bool                        saysAlgebra_ = false;
  clifford::Signature         saidAlgebra_;
  int                         algebraLine_ = -1;
  unsigned long               sharedVersion_ = 0;
  std::string                 sharedKey_;
  // Names the box mentions and does not define: what it may take from others.
  std::vector<std::string>    wanted_;
  // What it took, said as one string so that "has it changed" is a comparison.
  std::string                 importKey_;
  // The knobs it reads from other boxes, with their current numbers.
  std::vector<Literal>        imported_;
  std::string                 importedSource_;
  std::vector<std::string>    importedNames_;
  // Names this box mentions that are derived values in another box.
  std::vector<std::string>    derivedElsewhere_;
};

}  // namespace sb::mathprogram
