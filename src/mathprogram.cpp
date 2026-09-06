#include "mathprogram.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

#include "glslops.h"
#include "mathline.h"
#include "types/Clifford.h"
#include "types/RealFunction.h"
#include "types/examples.h"

namespace sb::mathprogram {
namespace {

constexpr int kRunTimeoutMs = 4000;

// Everything before the user's first line: the scaffolding the app supplies so
// that a fragment compiles. Diagnostics are shifted back by its length so they
// point at the line the user actually wrote.
//
// The types themselves are not defined here. Each one contributes the C++ that
// makes it exist (src/types/), and this file only knows where to put it.
std::string VizMeshSource();
std::string FormatFloat(float value);

std::string CxxPrologue(const clifford::Signature& sig,
                        const std::vector<std::string>& algebraTypes) {
  return std::string(
      "#include <cmath>\n"
      "#include <cstdio>\n"
      "#include <cstdlib>\n"
      "#include <functional>\n"
      "using std::sin; using std::cos; using std::tan; using std::sinh;\n"
      "using std::cosh; using std::tanh; using std::exp; using std::log;\n"
      "using std::sqrt; using std::pow; using std::fabs;\n"
      "static float sb_input(int argc, char** argv, int i, float fallback) {\n"
      "  return (i + 1 < argc) ? (float)std::atof(argv[i + 1]) : fallback;\n"
      "}\n") +
      // A canvas that defines a viz has its lines injected here, once per value
      // of that type, so the vocabulary they are written in has to be present.
      VizMeshSource() +
      // Everything a type adds to the language it is written in comes from the
      // type's own file, so the toolchain never has to know what one is.
      types::realfunction::Source() +
      // A Clifford algebra is written out only for the box that uses one, and
      // only the types that box turned out to mention. An ordinary box of
      // floats pays nothing for it.
      (algebraTypes.empty() ? std::string() : clifford::Prologue(sig, algebraTypes)) +
      "int main(int argc, char** argv) {\n";
}

// The same job as the C++ prologue, for the other language. A fragment shader
// wants the interface raylib gives it, one uniform per knob, and a main to put
// the user's lines in -- so the boilerplate goes on at both ends.
// What a viz box adds to C++: two ways of laying down geometry, and the value it
// is drawing. Triangles for a shape, lines for a path -- a path cannot be a
// ribbon of triangles here, because its width has to be constant on screen and
// the mesh is in value units, which the placing shader is free to stretch.
//
// `value` is the thing being drawn. Standing alone -- which is how a viz box is
// compiled, so that its mistakes are reported on its own lines -- it is an
// example of the type; injected into a program that actually has one, it is
// that one.
std::string VizCxxPrologue(const std::string& viz) {
  // `Type@Window` names one of a type's several vizes; what it is compiled
  // against is an example of the type, which is the half before the '@'.
  const std::string type = viz.substr(0, viz.find('@'));
  std::string dummy;
  if (type == types::realfunction::kName) {
    dummy = std::string(types::realfunction::kName) + " value = " +
            types::ExampleRealFunction("value") + ";\n";
  } else {
    dummy = "float value = " + FormatFloat(types::ExampleReal("value")) + "f;\n";
  }

  return std::string(
             "#include <cmath>\n"
             "#include <cstdio>\n"
             "#include <functional>\n"
             "#include <vector>\n"
             "using std::sin; using std::cos; using std::tan; using std::atan2;\n"
             "using std::exp; using std::log; using std::sqrt; using std::pow;\n"
             "using std::fabs;\n") +
         types::realfunction::Source() + VizMeshSource() + dummy + "int main() {\n";
}

// The half of that which a program *using* a value needs too, since the same
// lines are injected there.
std::string VizMeshSource() {
  return std::string(
      "static std::vector<float> sb_tris;\n"
      "static std::vector<float> sb_lines;\n"
      "static void vertex(float x, float y) { sb_tris.push_back(x); sb_tris.push_back(y); }\n"
      "static void line(float ax, float ay, float bx, float by) {\n"
      "  sb_lines.push_back(ax); sb_lines.push_back(ay);\n"
      "  sb_lines.push_back(bx); sb_lines.push_back(by);\n"
      "}\n"
      "static void sb_mesh(const char* name) {\n"
      "  std::printf(\"mesh.%s=\", name);\n"
      "  for (float f : sb_tris) std::printf(\"%.6g \", (double)f);\n"
      "  std::printf(\"\\n\");\n"
      "  std::printf(\"line.%s=\", name);\n"
      "  for (float f : sb_lines) std::printf(\"%.6g \", (double)f);\n"
      "  std::printf(\"\\n\");\n"
      "  sb_tris.clear(); sb_lines.clear();\n"
      "}\n");
}

std::string VizCxxEpilogue() {
  return std::string("  sb_mesh(\"\");\n  return 0;\n}\n");
}

// And its GLSL half is a *vertex* shader: the one that decides where each of
// those vertices ends up. The attribute and uniform names are raylib's, since
// the mesh is submitted through its batch.
std::string VizShaderPrologue(int* lines) {
  const std::string out =
      "#version 430\n"
      // No vertex attributes: the geometry is in the buffer the mesh half wrote,
      // and this reads it straight out of there. Nothing goes back to the CPU
      // between the two halves.
      "layout(std430, binding = 1) readonly buffer SbMesh { uint sbMeshN; float sbMeshV[]; };\n"
      "out vec2 fragTexCoord;\n"
      "out vec4 fragColor;\n"
      "uniform vec4 sbTint;\n"
      "uniform mat4 mvp;\n"
      // What the display window can tell a viz about the value it is drawing.
      "uniform float value;\n"     // the number itself, for a real
      "uniform vec4 sbArea;\n"     // where it draws: x, y, w, h, middle at the centre
      "uniform vec4 sbView;\n"     // what that rectangle spans: cx, cy, wide, high
      "uniform float sbScale;\n"   // how big chrome is drawn at this zoom
      // The operators the rewrite turns `a + b` into. A place shader is written
      // with operators like anything else on this canvas, so it needs them too.
      + glslops::BuiltinOverloads() +
      // Value space to screen. Provided rather than left to be written out,
      // because every viz needs it and a place box should be one line.
      "vec2 sbMap(vec2 v) {\n"
      "  return vec2(sbArea.x + sbArea.z * (0.5 + (v.x - sbView.x) / sbView.z),\n"
      "              sbArea.y + sbArea.w * (0.5 - (v.y - sbView.y) / sbView.w));\n"
      "}\n"
      "void main() {\n"
      "  fragTexCoord = vec2(0.0);\n"
      "  fragColor = sbTint;\n"
      "  vec2 p = vec2(sbMeshV[2 * gl_VertexID], sbMeshV[2 * gl_VertexID + 1]);\n";
  *lines = static_cast<int>(std::count(out.begin(), out.end(), '\n'));
  return out;
}

// A viz box has two halves and they are both GLSL now, so what tells them apart
// is what they do: the mesh half is the one that *lays geometry down*. Nothing
// else calls `vertex` or `line`, and a half that calls neither is the one that
// places what the other made.
bool LaysGeometryDown(const std::string& body) {
  return body.find("vertex(") != std::string::npos || body.find("line(") != std::string::npos;
}

// The mesh half, as a compute shader. One invocation: these are small meshes
// written as a loop, and laying them down in parallel would only mean sorting
// them again afterwards.
std::string VizComputePrologue() {
  return std::string(
             "#version 430\n"
             "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n"
             "layout(std430, binding = 1) buffer SbTris  { uint sbTriN;  float sbTriV[]; };\n"
             "layout(std430, binding = 2) buffer SbLines { uint sbLineN; float sbLineV[]; };\n"
             "uniform float value;\n") +
         glslops::BuiltinOverloads() +
         "const uint sbCap = " + std::to_string(Program::kVizVertexCap) +
         "u;\n"
         // The two ways of laying geometry down, and no more. A path is not a
         // ribbon of triangles because its width has to be constant on screen
         // while the mesh is in value units.
         "void vertex(float x, float y) {\n"
         "  uint i = atomicAdd(sbTriN, 1u);\n"
         "  if (i < sbCap) { sbTriV[2u * i] = x; sbTriV[2u * i + 1u] = y; }\n"
         "}\n"
         "void line(float ax, float ay, float bx, float by) {\n"
         "  uint i = atomicAdd(sbLineN, 2u);\n"
         "  if (i + 1u < sbCap) {\n"
         "    sbLineV[2u * i] = ax; sbLineV[2u * i + 1u] = ay;\n"
         "    sbLineV[2u * i + 2u] = bx; sbLineV[2u * i + 3u] = by;\n"
         "  }\n"
         "}\n"
         "void main() {\n";
}

std::string VizComputeEpilogue() { return "}\n"; }

std::string VizShaderEpilogue() {
  return std::string("  gl_Position = mvp * vec4(p, 0.0, 1.0);\n}\n");
}

std::string ShaderPrologue(const std::vector<Variable>& variables,
                           const clifford::Signature& sig,
                           const std::vector<std::string>& algebraTypes,
                           const std::string& functions,
                           const std::vector<std::string>& functionNames, int* lines) {
  // One shader, three modes. A box used to build three -- one to draw its
  // picture, one to report its values, one to draw its functions -- from the
  // same lines with different ends. They are the same shader with a uniform
  // saying which end to use, and building one instead of three is the only
  // thing that made a rebuild stutter.
  std::string out =
      "#version 430\n"
      "in vec2 sbVarying;\n"
      "in vec4 fragColor;\n"
      "out vec4 finalColor;\n"
      "uniform sampler2D texture0;\n"
      "uniform vec4 colDiffuse;\n"
      "uniform int sbMode;\n"       // 0 draw the picture, 1 report, 2 draw a function
      "uniform vec2 sbProbe;\n"     // the one point a report is about
      "uniform vec2 sbRange;\n"     // value at the left and right edges
      "uniform vec2 sbVertical;\n"  // value at the top and bottom edges
      "uniform int sbIndex;\n"      // which function
      "uniform int sbShape;\n"      // 0 a field on an axis, 1 a curve on a plot
      "uniform float sbThick;\n"    // half a curve's width, in value units
      "layout(std430, binding = 0) buffer SbTrace { float sbTrace[]; };\n";
  // A free real reaches a program through argv and a shader through a uniform.
  // Either way the knob on its number line is what moves it.
  // Not one uniform per variable -- one per *literal assigned to* a variable.
  // The variable itself is an ordinary mutable local, so it can be assigned
  // over; what a knob moves is whichever literal governs the line being read.
  for (const Variable& variable : variables) {
    if (variable.kind != Kind::Real) continue;
    for (const Literal& literal : variable.literals) {
      out += "uniform float " + literal.uniform + ";\n";
    }
  }
  // The same operations under the same names for the types GLSL already has,
  // so that rewriting `a * b` into `gp(a, b)` is safe on ordinary scalar and
  // vector code and nothing has to know what type anything is.
  out += glslops::BuiltinOverloads();
  // The same algebra the C++ gets, written for the other language. The named
  // forms are what exist there; the operators the user wrote are turned into
  // them before the body gets here.
  if (!algebraTypes.empty()) out += clifford::GlslPrologue(sig, algebraTypes);
  // The user's own functions, hoisted out of main because GLSL has nowhere else
  // to put them. One line per line of the box, so the block keeps the box's
  // shape even though most of its lines are blank.
  out += functions;

  // Which of the box's functions a field or a curve is drawing.
  out += "float sbEval(float x) {\n";
  for (std::size_t i = 0; i < functionNames.size(); ++i) {
    out += "  " + std::string(i == 0 ? "if" : "else if") + " (sbIndex == " + std::to_string(i) +
           ") return " + functionNames[i] + "(x);\n";
  }
  out +=
      "  return 0.0;\n"
      "}\n"
      "void main() {\n"
      // A function is drawn from the definitions alone, so this never runs the
      // body -- which is what keeps merging the three free rather than making
      // every pixel of a field pay for the whole box.
      "  if (sbMode == 2) {\n"
      "    float sbIn = sbShape == 0 ? mix(sbRange.x, sbRange.y, sbVarying.x)\n"
      "                              : mix(sbVertical.x, sbVertical.y, sbVarying.y);\n"
      "    float sbOut = sbEval(sbIn);\n"
      "    if (sbShape == 0) {\n"
      "      float v = clamp(sbOut, -1.0, 1.0);\n"
      "      vec3 green  = vec3(109.0 / 255.0, 205.0 / 255.0,  89.0 / 255.0);\n"
      "      vec3 yellow = vec3(253.0 / 255.0, 231.0 / 255.0,  37.0 / 255.0);\n"
      "      finalColor = vec4(v >= 0.0 ? green : yellow, abs(v) * fragColor.a);\n"
      "      return;\n"
      "    }\n"
      "    float here = mix(sbRange.x, sbRange.y, sbVarying.x);\n"
      "    float step = (sbVertical.y - sbVertical.x) * 0.0025;\n"
      "    float slope = (sbEval(sbIn + step) - sbEval(sbIn - step)) / (2.0 * step);\n"
      "    if (abs(here - sbOut) / sqrt(1.0 + slope * slope) > sbThick) discard;\n"
      "    finalColor = fragColor;\n"
      "    return;\n"
      "  }\n"
      // Reporting asks about one point rather than the one being drawn.
      "  vec2 ";
  out += kShaderInput;
  out += " = sbMode == 1 ? sbProbe : sbVarying;\n";
  *lines = static_cast<int>(std::count(out.begin(), out.end(), '\n'));
  return out;
}

// Ours rather than raylib's, so the texture coordinate arrives under a private
// name and the fragment half can choose between it and the probe point.
const char* const kVertexShader =
    "#version 430\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "out vec2 sbVarying;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  sbVarying = vertexTexCoord;\n"
    "  fragColor = vertexColor;\n"
    "  gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

// A shader *can* hand a value back: render the value instead of the picture,
// into a target one pixel per variable, and read the pixels. What it cannot do
// is hand back a value that does not depend on where you look -- inside a
// fragment shader a variable is a field, not a number -- so the point to read
// at is a uniform, and choosing it is the caller's problem.
//
// This is the same body over again with different ends: the varying replaced by
// that point, and the user's own write to finalColor overwritten by ours.
std::string ProbePrologue(const std::vector<Variable>& variables,
                          const clifford::Signature& sig,
                          const std::vector<std::string>& algebraTypes,
                          const std::string& functions) {
  // 430 rather than 330: this is the shader that writes the trace, and a
  // fragment shader cannot write to a shader storage buffer before then.
  std::string out =
      "#version 430\n"
      "in vec4 fragColor;\n"
      "out vec4 finalColor;\n"
      "uniform sampler2D texture0;\n"
      "uniform vec4 colDiffuse;\n"
      "uniform vec2 sbProbe;\n"
      "layout(std430, binding = 0) buffer SbTrace { float sbTrace[]; };\n";
  // Not one uniform per variable -- one per *literal assigned to* a variable.
  // The variable itself is an ordinary mutable local, so it can be assigned
  // over; what a knob moves is whichever literal governs the line being read.
  for (const Variable& variable : variables) {
    if (variable.kind != Kind::Real) continue;
    for (const Literal& literal : variable.literals) {
      out += "uniform float " + literal.uniform + ";\n";
    }
  }
  out += glslops::BuiltinOverloads();
  if (!algebraTypes.empty()) out += clifford::GlslPrologue(sig, algebraTypes);
  out += functions;
  out += "void main() {\n";
  // Not the interpolated varying: the one point being asked about.
  out += std::string("  vec2 ") + kShaderInput + " = sbProbe;\n";
  return out;
}

// A window drawing a function needs the box's functions and nothing else --
// not the body, which computes values, but the definitions, which are what a
// curve or a field is made of.
std::string FieldPrologue(const std::vector<Variable>& variables,
                          const clifford::Signature& sig,
                          const std::vector<std::string>& algebraTypes,
                          const std::string& functions) {
  std::string out =
      "#version 330\n"
      "in vec2 fragTexCoord;\n"
      "in vec4 fragColor;\n"
      "out vec4 finalColor;\n"
      "uniform sampler2D texture0;\n"
      "uniform vec4 colDiffuse;\n"
      "uniform vec2 sbRange;\n"      // value at the left and right edges
      "uniform vec2 sbVertical;\n"   // value at the top and bottom edges
      "uniform int sbIndex;\n"       // which of the box's functions
      "uniform int sbMode;\n"        // 0 a field on an axis, 1 a curve on a plot
      "uniform float sbThick;\n";    // half the curve's width, in value units
  // Not one uniform per variable -- one per *literal assigned to* a variable.
  // The variable itself is an ordinary mutable local, so it can be assigned
  // over; what a knob moves is whichever literal governs the line being read.
  for (const Variable& variable : variables) {
    if (variable.kind != Kind::Real) continue;
    for (const Literal& literal : variable.literals) {
      out += "uniform float " + literal.uniform + ";\n";
    }
  }
  out += glslops::BuiltinOverloads();
  if (!algebraTypes.empty()) out += clifford::GlslPrologue(sig, algebraTypes);
  out += functions;
  return out;
}

std::string FieldEpilogue(const std::vector<std::string>& names) {
  // Which of the box's functions, chosen once so the curve below can ask for
  // its value at three nearby inputs without the choice being made each time.
  std::string out = "float sbEval(float x) {\n";
  for (std::size_t i = 0; i < names.size(); ++i) {
    out += "  " + std::string(i == 0 ? "if" : "else if") + " (sbIndex == " + std::to_string(i) +
           ") return " + names[i] + "(x);\n";
  }
  out +=
      "  return 0.0;\n"
      "}\n";

  out +=
      "void main() {\n"
      "  float sbIn = sbMode == 0 ? mix(sbRange.x, sbRange.y, fragTexCoord.x)\n"
      "                           : mix(sbVertical.x, sbVertical.y, fragTexCoord.y);\n"
      "  float sbOut = sbEval(sbIn);\n";
  // Zero is nothing at all, +1 opaque viridis green, -1 opaque viridis yellow,
  // and beyond either end clamped rather than rescaled -- so the colour means
  // the same number on every axis it is drawn on.
  out +=
      "  if (sbMode == 0) {\n"
      "    float v = clamp(sbOut, -1.0, 1.0);\n"
      "    vec3 green  = vec3(109.0 / 255.0, 205.0 / 255.0,  89.0 / 255.0);\n"
      "    vec3 yellow = vec3(253.0 / 255.0, 231.0 / 255.0,  37.0 / 255.0);\n"
      "    finalColor = vec4(v >= 0.0 ? green : yellow, abs(v) * fragColor.a);\n"
      "    return;\n"
      "  }\n"
      // A curve is where the output actually is what the function says it is.
      "  float here = mix(sbRange.x, sbRange.y, fragTexCoord.x);\n"
      // How far the curve is, measured across it rather than along the row. A
      // fixed horizontal band would thin out to nothing wherever the curve is
      // steep, which is exactly where it is most worth seeing, so the distance
      // is divided by the slope -- one step either side, no calculus required.
      "  float step = (sbVertical.y - sbVertical.x) * 0.0025;\n"
      "  float slope = (sbEval(sbIn + step) - sbEval(sbIn - step)) / (2.0 * step);\n"
      "  float away = abs(here - sbOut) / sqrt(1.0 + slope * slope);\n"
      "  if (away > sbThick) discard;\n"
      "  finalColor = fragColor;\n"
      "}\n";
  return out;
}

std::string ProbeEpilogue(const std::vector<std::string>& probed) {
  std::string out =
      "  if (sbMode != 1) return;\n"
      "  float sbValue = 0.0;\n  int sbWhich = int(gl_FragCoord.x);\n";
  for (std::size_t i = 0; i < probed.size(); ++i) {
    out += "  if (sbWhich == " + std::to_string(i) + ") sbValue = " + probed[i] + ";\n";
  }
  // Exact, not scaled: the four bytes of the pixel are the four bytes of the
  // float, so nothing is lost on the way back.
  out +=
      "  uint sbBits = floatBitsToUint(sbValue);\n"
      "  finalColor = vec4(float((sbBits >> 24) & 255u), float((sbBits >> 16) & 255u),\n"
      "                    float((sbBits >> 8) & 255u), float(sbBits & 255u)) / 255.0;\n"
      "}\n";
  return out;
}

// Words only GLSL has. Used to decide which failure to report when the box
// compiles as neither language: whichever one the text was trying to be.
bool MentionsShaderWord(const std::string& text) {
  static const char* const kWords[] = {
      "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "bvec2", "bvec3", "bvec4",
      "mat2", "mat3", "mat4", "sampler2D", "fragTexCoord", "fragColor", "finalColor",
      "gl_FragCoord", "gl_Position", "gl_FragColor",
  };
  auto isWordChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
  };
  for (const char* word : kWords) {
    const std::string needle = word;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
      const bool beforeOk = (at == 0) || !isWordChar(text[at - 1]);
      const std::size_t end = at + needle.size();
      const bool afterOk = (end >= text.size()) || !isWordChar(text[end]);
      if (beforeOk && afterOk) return true;
    }
  }
  return false;
}

std::string FormatFloat(float value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(value));
  return buffer;
}

}  // namespace

int Program::LineOffset() {
  // Counted rather than kept in step by hand: the prologue grows as the
  // language does, and a stale offset points diagnostics at the wrong line.
  const std::string prologue = CxxPrologue(clifford::kDefaultSignature, {});
  return static_cast<int>(std::count(prologue.begin(), prologue.end(), '\n'));
}

Program::~Program() {
  // Wait for anything in flight before the futures go out of scope -- and
  // *take* the build's result rather than only waiting for it. A compile that
  // finished after nobody wanted it any more still wrote an executable, and
  // dropping the result unread would leave that file behind for good.
  if (build_.valid()) {
    const compiler::Build build = build_.get();
    compiler::Discard(build.exePath);
  }
  if (run_.valid()) run_.wait();
  compiler::Discard(exePath_);
}

void Program::Rebuild(const std::vector<std::string>& lines, const VizSources* vizes) {
  if (build_.valid()) {
    // One build at a time -- but remember that the ask happened, or it is lost.
    rebuildQueued_ = true;
    queuedLines_ = lines;
    queuedVizes_ = vizes;
    return;
  }
  rebuildQueued_ = false;

  // Remember what the knobs were set to, so an edit does not reset them.
  std::unordered_map<std::string, float> previous;
  for (const Variable& variable : variables_) {
    if (variable.free) previous.emplace(variable.name, variable.value);
  }

  // Which algebra this box works in, and which of its types it mentions. Both
  // are read off the box itself: `Algebra(3,0,1);` on a line of its own changes
  // the signature, and everything else defaults to 3D projective geometric
  // algebra, which is what these tools are mostly for.
  clifford::Signature signature = clifford::kDefaultSignature;
  std::string         joined;
  // And what, if anything, this box draws. `Viz(Real);` makes it a viz: it says
  // how a value of that type is shown rather than declaring one, so it is
  // assembled entirely differently from here on.
  vizType_.clear();
  for (const std::string& raw : lines) {
    clifford::ParseAlgebra(raw, &signature);
    if (std::string drawn; mathline::Parse(raw).kind == mathline::Kind::Viz) {
      vizType_ = mathline::Parse(raw).name;
    }
    joined += raw;
    joined += '\n';
  }
  const std::vector<std::string> algebraTypes = clifford::TypesMentioned(joined, signature);

  std::vector<Variable> variables;
  std::string body;
  // The same lines assembled for the other language, kept in step line for line
  // with the C++ so a diagnostic from either lands on the same row of the box.
  std::string shaderBody;
  // GLSL has no nested functions, so a function the user writes cannot stay
  // where they wrote it: it is hoisted above main. The block is padded to one
  // line per line of the box, so the two halves keep the same shape and a line
  // number is still a line number.
  std::string shaderFunctions;
  int freeIndex = 0;
  // One number the trace carries: what to write, whose it is, and the line it
  // comes into scope on. A capture placed before its variable exists would not
  // compile, so the trace is built to match.
  struct Slot {
    std::string expression;   // what the shader writes
    std::string owner;
    int         component = -1;   // -1 for a real
    int         line = 0;
  };
  std::vector<Slot> liveFrom;
  // Every component of a multivector, since one is not a number.
  const auto addSlots = [&](const Variable& variable, int at) {
    if (variable.kind == Kind::Real) {
      liveFrom.push_back({variable.name, variable.name, -1, at});
      return;
    }
    if (variable.kind != Kind::Multivector) return;
    const std::vector<std::string> members = clifford::MemberNames(variable.type, signature);
    for (std::size_t i = 0; i < members.size(); ++i) {
      liveFrom.push_back({variable.name + "." + members[i], variable.name,
                          static_cast<int>(i), at});
    }
  };
  int lineIndex = 0;

  for (const std::string& raw : lines) {
    const int thisLine = lineIndex++;
    (void)thisLine;
    // The line that chooses the algebra is a statement about the box, not a
    // statement in it. It leaves a blank behind rather than vanishing, so a
    // diagnostic still lands on the row the user wrote.
    if (clifford::ParseAlgebra(raw, nullptr)) {
      body += "\n";
      shaderBody += "\n";
      shaderFunctions += "\n";
      continue;
    }

    const mathline::Line line = mathline::Parse(raw, signature);
    // So does the line that says what this box draws.
    if (line.kind == mathline::Kind::Viz) {
      body += "\n";
      shaderBody += "\n";
      shaderFunctions += "\n";
      continue;
    }

    if (line.kind == mathline::Kind::Declaration && !line.hasInitialiser) {
      // The abstract declaration the user wrote; the initialiser is ours.
      Variable variable;
      variable.declaredLine = thisLine;
      variable.name = line.name;
      variable.type = line.type;
      variable.latexName = mathline::NameToLatex(line.name);
      variable.kind = mathline::IsFunctionType(line.type)
                          ? Kind::Function
                          : (mathline::IsMultivectorType(line.type, signature) ? Kind::Multivector
                                                                              : Kind::Real);
      variable.free = true;

      if (variable.kind == Kind::Multivector) {
        // A multivector cannot travel down argv as one number, so its chosen
        // value is written into the source the way a function's is. That makes
        // choosing one a rebuild rather than a re-run, which is why it has no
        // knob yet.
        variables.push_back(variable);
        addSlots(variable, thisLine);
        // The chosen value is written into the source, in the one spelling both
        // languages accept -- so the shader gets the declaration too, rather
        // than a blank where its value should be.
        const std::string declaration = "  " + line.type + " " + line.name + " = " +
                                        clifford::ExampleValue(line.type, line.name, signature) +
                                        ";\n";
        body += declaration;
        shaderBody += declaration;
        shaderFunctions += "\n";
        continue;
      }

      if (variable.kind == Kind::Function) {
        // A function cannot travel down argv, so choosing one means writing it
        // into the source. That makes it a rebuild rather than a re-run, which
        // is why a function has no knob yet.
        variables.push_back(variable);
        body += "  " + line.type + " " + line.name + " = " +
                types::ExampleRealFunction(line.name) + ";\n";
        // In GLSL the chosen function is a definition, hoisted above main.
        shaderFunctions += types::realfunction::ExampleGlsl(line.name) + "\n";
        shaderBody += "\n";
        continue;
      }

      variable.example = types::ExampleReal(line.name);
      auto found = previous.find(line.name);
      if (found != previous.end()) {
        variable.value = found->second;
      } else {
        // Nothing on screen for it: fall back to a loaded value, then to the
        // example the type generated.
        variable.value = variable.example;
        for (const auto& [name, value] : restore_) {
          if (name == line.name) { variable.value = value; break; }
        }
      }
      // The literal the app injects, standing behind a uniform of its own.
      Literal injected;
      injected.line = thisLine;
      injected.uniform = variable.name + "_" + std::to_string(thisLine);
      injected.value = variable.value;
      variable.literals.push_back(injected);

      variables.push_back(variable);
      addSlots(variable, thisLine);

      body += "  " + line.type + " " + line.name + " = sb_input(argc, argv, " +
              std::to_string(freeIndex) + ", 0.0f);\n";
      // An ordinary local, initialised from the uniform. Not a uniform itself:
      // a uniform cannot be assigned over, and a variable in a box can.
      shaderBody += "  float " + line.name + " = " + injected.uniform + ";\n";
      ++freeIndex;
      continue;
    }

    if (line.kind == mathline::Kind::Declaration && line.hasInitialiser) {
      Variable variable;
      variable.declaredLine = thisLine;
      variable.name = line.name;
      variable.type = line.type;
      variable.latexName = mathline::NameToLatex(line.name);
      variable.kind = mathline::IsFunctionType(line.type)
                          ? Kind::Function
                          : (mathline::IsMultivectorType(line.type, signature) ? Kind::Multivector
                                                                              : Kind::Real);
      variable.free = false;
      variables.push_back(variable);
      addSlots(variable, thisLine);

      // A function written out is a GLSL function definition already -- only
      // its type name differs, because `RealFunction` is what says *which*
      // arrow it is and GLSL only knows it returns a float. So it is hoisted
      // above main with the name changed and nothing else touched.
      if (variable.kind == Kind::Function) {
        const std::size_t at = raw.find(line.type);
        std::string       definition = raw;
        if (at != std::string::npos) definition.replace(at, line.type.size(), "float");
        shaderFunctions += definition + "\n";
        shaderBody += "\n";
        body += "  " + raw + "\n";
        continue;
      }
    }

    // `foo = 3.5;` -- a bare number on the right, which is a *fixed value*
    // rather than a rule, exactly the distinction `=` and `:=` already draw.
    // It gets a uniform of its own so it can be dragged where it governs.
    if (line.kind == mathline::Kind::Assignment && line.marks.size() == 1) {
      bool handled = false;
      for (Variable& variable : variables) {
        if (variable.name != line.name || variable.kind != Kind::Real) continue;
        Literal literal;
        literal.line = thisLine;
        literal.uniform = variable.name + "_" + std::to_string(thisLine);
        const std::size_t at = raw.find('=');
        literal.value =
            (at == std::string::npos) ? 0.0f : std::strtof(raw.c_str() + at + 1, nullptr);
        variable.literals.push_back(literal);

        body += "  " + raw + "\n";
        shaderBody += "  " + variable.name + " = " + literal.uniform + ";\n";
        shaderFunctions += "\n";
        handled = true;
        break;
      }
      if (handled) continue;
    }

    // Anything else is passed through verbatim: it is already valid C++, or it
    // is the user's mistake to see reported.
    body += "  " + raw + "\n";
    shaderBody += "  " + raw + "\n";
    shaderFunctions += "\n";
  }

  // A viz box has no variables and wants none: what it produces is a mesh, or
  // the shader that places one. Both halves are assembled from the same lines,
  // exactly as an ordinary box's two languages are, and which one the box turns
  // out to be is decided the same way -- C++ first, GLSL if C++ refuses it.
  if (!vizType_.empty()) {
    variables_.clear();
    probed_.clear();
    vizMesh_.clear();

    // Both halves are GLSL; which one this is depends on what it does.
    std::string vizBody = shaderBody;
    glslops::Rewrite(shaderBody, &vizBody, nullptr);

    glslMesh_.clear();
    int vizPrologueLines = 0;
    if (LaysGeometryDown(shaderBody)) {
      const std::string head = VizComputePrologue();
      glslMesh_ = head + vizBody + VizComputeEpilogue();
      vizPrologueLines = static_cast<int>(std::count(head.begin(), head.end(), '\n'));
      // A mesh half draws nothing itself, so it has no place-shader to offer.
      glsl_.clear();
    } else {
      glsl_ = VizShaderPrologue(&vizPrologueLines) + vizBody + VizShaderEpilogue();
    }
    // A viz box has no hoisted functions, so both halves of the map point at
    // the same place.
    glslLines_ = {vizPrologueLines, vizPrologueLines, static_cast<int>(lines.size())};
    looksLikeGlsl_ = true;   // a viz box is GLSL either way now

    // Shading means "the host should compile this on the thread that owns the
    // graphics context", which is the only way a box is built now.
    state_ = State::Shading;
    error_ = {};
    detail_.clear();
    return;
  }

  if (variables.empty()) {
    variables_.clear();
    state_ = State::Empty;
    compiler::Discard(exePath_);
    exePath_.clear();
    // A box with nothing in it should be holding nothing.
    glsl_.clear();
    probed_.clear();
    return;
  }

  for (const Variable& variable : variables) {
    if (variable.kind == Kind::Multivector) {
      // Its components, in the order the struct declares them. One line of
      // numbers, like everything else printed back.
      const std::vector<std::string> members =
          clifford::MemberNames(variable.type, signature);
      if (members.empty()) continue;
      body += "  std::printf(\"" + variable.name + "=";
      for (std::size_t i = 0; i < members.size(); ++i) body += (i == 0 ? "%.9g" : " %.9g");
      body += "\\n\"";
      for (const std::string& member : members) {
        body += ", (double)" + variable.name + "." + member;
      }
      body += ");\n";
      continue;
    }
    if (variable.kind == Kind::Function) {
      body += types::realfunction::PrintStatement(variable.name);
    } else {
      body += "  std::printf(\"" + variable.name + "=%.9g\\n\", (double)" + variable.name +
              ");\n";
    }

    // And, if the canvas says how this type is drawn, its lines run here -- in
    // the only program that has the value, which for a function is the only
    // place it can be sampled at all. Sampling and building the mesh are then
    // the same editable snippet, and they cost nothing extra: this program was
    // going to be run anyway.
    if (vizes == nullptr) continue;
    const auto found = vizes->find(variable.type);
    if (found == vizes->end() || found->second.empty()) continue;
    body += "  {\n    const auto& value = " + variable.name + "; (void)value;\n";
    for (const std::string& raw : found->second) body += "    " + raw + "\n";
    body += "    sb_mesh(\"" + variable.name + "\");\n  }\n";
  }
  body += "  return 0;\n}\n";

  // Assembled now, alongside the C++, and used only if the C++ is refused.
  //
  // The operators go first. GLSL has none to overload, so `a * b` is rewritten
  // into `gp(a, b)` -- which needs no types, because GLSL resolves the overload
  // itself. Line breaks survive it, so a driver diagnostic still lands on the
  // line the user wrote.
  std::string rewritten = shaderBody;
  glslops::Rewrite(shaderBody, &rewritten, nullptr);
  std::string functions = shaderFunctions;
  glslops::Rewrite(shaderFunctions, &functions, nullptr);

  functionNames_.clear();
  for (const Variable& variable : variables) {
    if (variable.kind == Kind::Function) functionNames_.push_back(variable.name);
  }
  probed_.clear();
  for (const Variable& variable : variables) {
    if (!variable.free && variable.kind == Kind::Real) probed_.push_back(variable.name);
  }

  int prologueLines = 0;
  const std::string head =
      ShaderPrologue(variables, signature, algebraTypes, functions, functionNames_,
                     &prologueLines);
  // The box's lines appear twice: hoisted above main, and again inside it.
  glslLines_ = {prologueLines - static_cast<int>(lines.size()) - 1, prologueLines,
                static_cast<int>(lines.size())};
  // The box's lines appear twice: hoisted above main, and again inside it.
  glslLines_ = {prologueLines - static_cast<int>(lines.size()) - 1, prologueLines,
                static_cast<int>(lines.size())};
  looksLikeGlsl_ = MentionsShaderWord(shaderBody);

  // The trace: after each line, what every real in scope is at that point. It
  // rides on the end of the line rather than on a line of its own, so the line
  // numbering a diagnostic depends on is untouched.
  tracedSlots_.clear();
  for (const Slot& slot : liveFrom) tracedSlots_.emplace_back(slot.owner, slot.component);
  tracedLines_ = static_cast<int>(lines.size());
  trace_.clear();

  std::string traced;
  {
    std::size_t begin = 0;
    int         k = 0;
    while (begin <= rewritten.size() && k < tracedLines_) {
      const std::size_t end = rewritten.find('\n', begin);
      traced += rewritten.substr(begin, (end == std::string::npos ? rewritten.size() : end) - begin);
      std::string captures;
      for (std::size_t i = 0; i < liveFrom.size(); ++i) {
        if (liveFrom[i].line > k) continue;
        captures += " sbTrace[" + std::to_string(k * static_cast<int>(liveFrom.size()) +
                                                 static_cast<int>(i)) +
                    "] = " + liveFrom[i].expression + ";";
      }
      // Guarded, because the same shader draws the picture: without this every
      // pixel of every frame would be racing to write the trace.
      if (!captures.empty()) traced += " if (sbMode == 1) {" + captures + " }";
      traced += "\n";
      if (end == std::string::npos) break;
      begin = end + 1;
      ++k;
    }
  }

  glsl_ = head + traced + ProbeEpilogue(probed_);

  variables_ = std::move(variables);
  // There is one language. A box is GLSL, and the host compiles it on the
  // thread that owns the graphics context -- which is what Shading means.
  state_ = State::Shading;
  error_ = {};
  detail_.clear();
}

void Program::ShaderCompiled(bool ok, const std::string& log) {
  if (state_ != State::Shading) return;
  if (ok) {
    state_ = State::ShaderReady;
    error_ = {};
    return;
  }

  // Neither language would take it. Report the failure from whichever one the
  // text was trying to be, and C++ if it does not look like either.
  state_ = State::Failed;
  const compiler::Diagnostic shaderError = compiler::ParseGlslLog(log, glslLines_);
  error_ = (looksLikeGlsl_ && shaderError.present) ? shaderError : cppError_;
}

void Program::SetProbedValues(const std::vector<float>& values) {
  if (state_ != State::ShaderReady) return;
  // Walked in the same order probed_ was built in, so the n-th pixel is the
  // n-th derived real.
  std::size_t index = 0;
  for (Variable& variable : variables_) {
    if (variable.free || variable.kind != Kind::Real) continue;
    if (index >= values.size()) break;
    if (std::isfinite(values[index])) {
      variable.value = values[index];
      variable.known = true;
    }
    ++index;
  }
}

void Program::Restore(std::vector<std::pair<std::string, float>> values) {
  restore_ = std::move(values);
}

const Literal* Program::FindLiteral(const std::string& uniform) const {
  for (const Variable& variable : variables_) {
    for (const Literal& literal : variable.literals) {
      if (literal.uniform == uniform) return &literal;
    }
  }
  return nullptr;
}

void Program::SetValue(std::size_t index, float value, int atLine) {
  if (index >= variables_.size()) return;
  Variable& variable = variables_[index];
  if (variable.kind != Kind::Real) return;

  // The literal responsible for what is being looked at is the one that moves.
  const int which = variable.GoverningLiteral(atLine);
  if (which < 0) return;
  variable.literals[static_cast<std::size_t>(which)].value = value;
  variable.value = value;
}

void Program::Launch() {
  if (state_ != State::Ready || exePath_.empty()) return;
  if (run_.valid()) {
    // A run is already going; remember that the inputs moved on.
    runQueued_ = true;
    return;
  }

  std::vector<std::string> arguments;
  for (const Variable& variable : variables_) {
    if (variable.free && variable.kind == Kind::Real) {
      arguments.push_back(FormatFloat(variable.value));
    }
  }

  const std::string exe = exePath_;
  run_ = std::async(std::launch::async, [exe, arguments] {
    RunOutput output;
    const compiler::Run run = compiler::Execute(exe, arguments, kRunTimeoutMs);
    if (!run.ok) return output;

    // The program prints one `name=` per line, followed by its numbers: one for
    // a real, a whole sample run for a function.
    std::size_t start = 0;
    while (start <= run.output.size()) {
      std::size_t end = run.output.find('\n', start);
      if (end == std::string::npos) end = run.output.size();
      std::string_view text(run.output.data() + start, end - start);
      if (!text.empty() && text.back() == '\r') text.remove_suffix(1);

      const std::size_t equals = text.find('=');
      if (equals != std::string_view::npos) {
        RunValue value;
        value.name = std::string(text.substr(0, equals));
        const std::string numbers(text.substr(equals + 1));
        const char* cursor = numbers.c_str();
        while (*cursor != '\0') {
          char* after = nullptr;
          const float number = std::strtof(cursor, &after);
          if (after == cursor) break;
          value.numbers.push_back(number);
          cursor = after;
        }
        output.values.push_back(std::move(value));
      }
      if (end == run.output.size()) break;
      start = end + 1;
    }
    output.ok = true;
    return output;
  });
}

void Program::Poll() {
  if (build_.valid() &&
      build_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const compiler::Build build = build_.get();
    build_ = {};

    if (!build.haveCompiler) {
      state_ = State::NoCompiler;
      detail_ = build.detail;
    } else if (!build.ok) {
      // C++ would not have it. The same lines might still be a shader, but
      // compiling one means talking to the graphics context, which is not ours
      // to talk to: hand the source over and wait to be told what happened.
      cppError_ = build.error;
      state_ = State::Shading;
    } else {
      compiler::Discard(exePath_);
      exePath_ = build.exePath;
      state_ = State::Ready;
      // C++ took it, so the shader it might have been is not wanted. It is
      // reassembled from scratch if a later edit needs it.
      glsl_.clear();
        probed_.clear();
      Launch();  // first run, to fill in the derived values
    }

    if (rebuildQueued_) {
      rebuildQueued_ = false;
      Rebuild(queuedLines_, queuedVizes_);
      return;
    }
  }

  if (run_.valid() && run_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const RunOutput output = run_.get();
    run_ = {};

    if (output.ok) {
      for (const RunValue& printed : output.values) {
        // A viz box standing alone prints one mesh, for its example value.
        if (!vizType_.empty() && printed.name == "mesh.") {
          vizMesh_ = printed.numbers;
          continue;
        }
        if (!vizType_.empty() && printed.name == "line.") {
          vizLines_ = printed.numbers;
          continue;
        }
        // A program that has values prints one mesh per value.
        if (printed.name.rfind("mesh.", 0) == 0 || printed.name.rfind("line.", 0) == 0) {
          const bool tris = printed.name[0] == 'm';
          const std::string owner = printed.name.substr(5);
          for (Variable& variable : variables_) {
            if (variable.name != owner) continue;
            (tris ? variable.mesh : variable.lines) = printed.numbers;
            break;
          }
          continue;
        }
        for (Variable& variable : variables_) {
          if (variable.name != printed.name) continue;
          if (variable.kind == Kind::Function) {
            // Nothing on screen owns a function's samples, free or not: the
            // program is the only thing that knows what the curve looks like.
            variable.samples = printed.numbers;
            variable.known = !variable.samples.empty();
          } else if (variable.kind == Kind::Multivector) {
            variable.components = printed.numbers;
            variable.known = !variable.components.empty();
          } else if (!printed.numbers.empty()) {
            // A free real is owned by its knob; only take derived values back,
            // or the answer from a run that has already been superseded would
            // yank the slider backwards.
            if (!variable.free) variable.value = printed.numbers.front();
            variable.known = true;
          }
          break;
        }
      }
    }

    if (runQueued_) {
      runQueued_ = false;
      Launch();
    }
  }
}

void Program::TakeTrace(std::vector<float> values) { trace_ = std::move(values); }

void Program::ApplyTrace(int line) {
  const int slots = static_cast<int>(tracedSlots_.size());
  if (slots == 0 || tracedLines_ == 0) return;
  if (static_cast<int>(trace_.size()) < tracedLines_ * slots) return;

  // Off the end -- and a box nobody has touched -- is read at the end, which is
  // what the whole block came to.
  const int at = (line < 0 || line >= tracedLines_) ? tracedLines_ - 1 : line;
  for (int i = 0; i < slots; ++i) {
    const float value = trace_[static_cast<std::size_t>(at * slots + i)];
    if (!std::isfinite(value)) continue;
    const auto& [owner, component] = tracedSlots_[static_cast<std::size_t>(i)];
    for (Variable& variable : variables_) {
      if (variable.name != owner) continue;
      if (component < 0) {
        variable.value = value;
        variable.known = true;
      } else {
        // A multivector is a fixed, short list of numbers rather than one.
        if (static_cast<int>(variable.components.size()) <= component) {
          variable.components.resize(static_cast<std::size_t>(component) + 1, 0.0f);
        }
        variable.components[static_cast<std::size_t>(component)] = value;
        variable.known = true;
      }
      break;
    }
  }
}

const std::string& Program::glslVertexSource() const {
  static const std::string source = kVertexShader;
  return source;
}

}  // namespace sb::mathprogram
