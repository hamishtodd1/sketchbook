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
#include "types/Complex.h"
#include "types/RealFunction.h"
#include "types/examples.h"

namespace sb::mathprogram {

int Dimension(Space space) {
  switch (space) {
    case Space::R1: return 1;
    case Space::R2: return 2;
    case Space::C: return 2;
    case Space::R3: return 3;
    default: return 1;
  }
}

namespace {

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

// The same line with its leading type name taken off: `float c = a + b;`
// becomes `c = a + b;`. A box's variables are globals now, so the line that
// used to declare one only assigns to it.
std::string WithoutType(const std::string& raw, const std::string& type) {
  const std::size_t at = raw.find(type);
  if (at == std::string::npos) return raw;
  std::size_t after = at + type.size();
  while (after < raw.size() && (raw[after] == ' ' || raw[after] == '\t')) ++after;
  return raw.substr(0, at) + raw.substr(after);
}

// Which space a type is. See the note beside `Space`: this one function is what
// decides which window a function is drawn in and how it is coloured.
Space SpaceOf(const std::string& type, const clifford::Signature& sig) {
  if (mathline::IsComplexType(type)) return Space::C;
  if (type == "vec2") return Space::R2;
  if (type == "vec3") return Space::R3;
  if (mathline::GlslTypeName(type) == "float") return Space::R1;
  if (mathline::IsMultivectorType(type, sig)) return Space::Multi;
  return Space::None;
}

// How many numbers one value of a type takes. A real is one; a multivector is
// one per component, in the order its struct declares them.
int ComponentCount(const std::string& type, const clifford::Signature& sig) {
  if (mathline::IsMultivectorType(type, sig)) {
    return static_cast<int>(clifford::MemberNames(type, sig).size());
  }
  if (mathline::IsComplexType(type)) {
    return static_cast<int>(types::complexnum::MemberNames().size());
  }
  return Dimension(SpaceOf(type, sig));
}

// How a picture is coloured, which is a question about the *output* alone. Only
// the ones a box actually needs are written out.
std::string PaintSource(const std::vector<Function>& fns) {
  bool real = false, colour = false, complex = false;
  for (const Function& fn : fns) {
    if (!fn.IsDrawn()) continue;
    real = real || fn.out == Space::R1;
    colour = colour || fn.out == Space::R3;
    complex = complex || fn.out == Space::C;
  }
  std::string out;
  if (real) {
    // The same map a function of one variable gets along a number line, so the
    // colour means the same number whichever window it is read in.
    out +=
        "vec4 sbPaint(float v) {\n"
        "  float t = clamp(v, -1.0, 1.0);\n"
        "  vec3 green  = vec3(109.0 / 255.0, 205.0 / 255.0,  89.0 / 255.0);\n"
        "  vec3 yellow = vec3(253.0 / 255.0, 231.0 / 255.0,  37.0 / 255.0);\n"
        "  return vec4(mix(vec3(0.0), t >= 0.0 ? green : yellow, abs(t)), 1.0);\n"
        "}\n";
  }
  if (colour) out += "vec4 sbPaint(vec3 c) { return vec4(c, 1.0); }\n";
  if (real) {
    out +=
        "vec3 sbTint(float v) {\n"
        "  vec3 green  = vec3(109.0 / 255.0, 205.0 / 255.0,  89.0 / 255.0);\n"
        "  vec3 yellow = vec3(253.0 / 255.0, 231.0 / 255.0,  37.0 / 255.0);\n"
        "  return v >= 0.0 ? green : yellow;\n"
        "}\n"
        "float sbThickness(float v) { return clamp(abs(v), 0.0, 1.0); }\n";
  }
  if (complex) {
    // Domain colouring, the way it is always done: the argument is the hue and
    // the size is the lightness, black at a zero and white as it runs away. Both
    // halves of the answer are on screen at once, which is the whole reason to
    // hold C apart from R2.
    out +=
        "vec3 sbHue(float h) {\n"
        "  vec3 k = fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0));\n"
        "  return clamp(abs(k * 6.0 - 3.0) - 1.0, 0.0, 1.0);\n"
        "}\n"
        "vec4 sbPaint(Complex z) {\n"
        "  float a = carg(z) / 6.2831853 + 0.5;\n"
        "  float m = cabs(z);\n"
        "  float l = m / (1.0 + m);\n"
        "  vec3 hue = sbHue(a);\n"
        "  vec3 rgb = l < 0.5 ? hue * (l * 2.0) : mix(hue, vec3(1.0), (l - 0.5) * 2.0);\n"
        "  return vec4(rgb, 1.0);\n"
        "}\n"
        "vec3 sbTint(Complex z) { return sbHue(carg(z) / 6.2831853 + 0.5); }\n"
        "float sbThickness(Complex z) { float m = cabs(z); return m / (1.0 + m); }\n";
  }
  return out;
}

// A function of two variables is a picture: one pixel per input, coloured by
// what comes out. The same rule the field along a number line already follows,
// with two variables instead of one.
std::string PictureDispatch(const std::vector<Function>& fns) {
  std::string branches;
  bool        any = false;
  for (std::size_t i = 0; i < fns.size(); ++i) {
    if (!fns[i].IsPicture()) continue;
    const std::string arg = (fns[i].in == Space::C) ? "Complex(sbP.x, sbP.y)" : "sbP";
    branches += "    " + std::string(any ? "else if" : "if") + " (sbIndex == " +
                std::to_string(i) + ") { finalColor = sbPaint(" + fns[i].name + "(" + arg +
                ")); return; }\n";
    any = true;
  }
  if (!any) return {};
  return "  if (sbMode == 2 && sbShape == 2) {\n"
         "    vec2 sbP = vec2(mix(sbRange.x, sbRange.y, sbVarying.x),\n"
         "                    mix(sbVertical.x, sbVertical.y, sbVarying.y));\n" +
         branches + "    discard;\n  }\n";
}

// The same functions again, as height. A surface is ray-marched rather than
// meshed: the march is a dozen lines in the shader that is already there, where
// a mesh would be a vertex buffer, a second program and a draw path of its own
// -- and the same march is what a volume will want.
//
// **Orthographic, not perspective.** Height here is a quantity, and under a
// perspective the same quantity would draw a different size depending on where
// it stood. The other 3D window shows *points*, where perspective is what tells
// you which is nearer; this one shows a graph, where it would only lie.
// Where the eye is and which way is up, shared by everything with a camera of
// its own. Orthographic in both: these are graphs, and under a perspective the
// same quantity would draw a different size depending on where it stood.
std::string EyeSource() {
  return
      "vec3 sbEye() {\n"
      "  return vec3(-sin(sbOrbit.x) * cos(sbOrbit.y), sin(sbOrbit.y),\n"
      "              cos(sbOrbit.x) * cos(sbOrbit.y));\n"
      "}\n"
      "void sbFrame(out vec3 fwd, out vec3 rt, out vec3 up) {\n"
      "  fwd = -sbEye();\n"
      // Straight down leaves 'which way is up' with no answer, so it is given
      // one rather than left to a normalize of nothing.
      "  vec3 wup = abs(fwd.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);\n"
      "  rt = normalize(cross(fwd, wup));\n"
      "  up = cross(rt, fwd);\n"
      "}\n"
      "vec2 sbFilm() { return vec2(sbVarying.x, 1.0 - sbVarying.y) * 2.0 - 1.0; }\n"
      "vec2 sbMid() {\n"
      "  return vec2((sbRange.x + sbRange.y) * 0.5, (sbVertical.x + sbVertical.y) * 0.5);\n"
      "}\n";
}

// A function of three variables, drawn by looking through it. The same march as
// the surface, accumulating instead of stopping: colour by what the value is,
// opacity by how much of it there is, front to back so what is nearer hides
// what is behind it.
std::string VolumeDispatch(const std::vector<Function>& fns) {
  std::string branches;
  bool        any = false;
  for (std::size_t i = 0; i < fns.size(); ++i) {
    if (!fns[i].IsVolume()) continue;
    const std::string call = fns[i].name + "(q)";
    branches += "    " + std::string(any ? "else if" : "if") + " (sbIndex == " +
                std::to_string(i) + ") { vec3 t = sbTint(" + call +
                "); return vec4(t, sbThickness(" + call + ")); }\n";
    any = true;
  }
  if (!any) return {};

  return "vec4 sbCloud(vec3 q) {\n" + branches +
         "  return vec4(0.0);\n"
         "}\n"
         // **Perspective**, where the surface is orthographic. A surface is a
         // graph, and a graph must draw the same quantity the same size wherever
         // it stands. A volume is a place you are looking *into*, and there the
         // near things being bigger is exactly what says they are near -- take
         // it away and a cloud has no depth in it at all. The march is clipped
         // to the box first, so its steps are spent where something could be.
         "vec4 sbVolume() {\n"
         "  vec3 fwd, rt, up;\n"
         "  sbFrame(fwd, rt, up);\n"
         "  vec2  mid = sbMid();\n"
         "  float sbHalf = abs(sbRange.y - sbRange.x) * 0.5;\n"
         "  vec2  uv = sbFilm();\n"
         "  vec3  hub = vec3(mid.x, mid.y, 0.0);\n"
         "  vec3  eye = hub + sbEye() * sbOrbit.z * sbHalf;\n"
         "  float wide = tan(0.4);\n"
         "  vec3  dir = normalize(fwd + (uv.x * rt + uv.y * up) * wide);\n"
         "  vec3 inv = 1.0 / dir;\n"
         "  vec3 t0 = (hub - vec3(sbHalf) - eye) * inv;\n"
         "  vec3 t1 = (hub + vec3(sbHalf) - eye) * inv;\n"
         "  vec3 tn = min(t0, t1);\n"
         "  vec3 tf = max(t0, t1);\n"
         "  float in0 = max(max(max(tn.x, tn.y), tn.z), 0.0);\n"
         "  float out0 = min(min(tf.x, tf.y), tf.z);\n"
         "  if (out0 <= in0) return vec4(0.0);\n"
         "  float sbStep = (out0 - in0) / 96.0;\n"
         "  vec4  acc = vec4(0.0);\n"
         "  for (int i = 0; i < 96; ++i) {\n"
         "    vec3 q = eye + dir * (in0 + (float(i) + 0.5) * sbStep) - hub;\n"
         "    vec4 c = sbCloud(q);\n"
         "    float a = 1.0 - exp(-c.a * sbStep * 6.0);\n"
         "    acc.rgb += (1.0 - acc.a) * c.rgb * a;\n"
         "    acc.a += (1.0 - acc.a) * a;\n"
         "    if (acc.a > 0.995) break;\n"
         "  }\n"
         "  return acc;\n"
         "}\n";
}

std::string SurfaceDispatch(const std::vector<Function>& fns) {
  std::string branches;
  bool        any = false;
  for (std::size_t i = 0; i < fns.size(); ++i) {
    if (!fns[i].IsSurface()) continue;
    branches += "      " + std::string(any ? "else if" : "if") + " (sbIndex == " +
                std::to_string(i) + ") return clamp(" + fns[i].name +
                "(q), -sbLift, sbLift);\n";
    any = true;
  }
  if (!any) return {};

  return "float sbHeight(vec2 q) {\n" + branches +
         "  return 0.0;\n"
         "}\n"
         // One ray, from a point on the film. `nearest` comes back as how close,
         // in pixels, it passed to the surface *before* the approach it landed
         // on -- the two steps leading into a crossing are always close, and
         // are not an edge.
         "vec4 sbSurfaceRay(vec2 uv, float pixel, out float nearest) {\n"
         "  vec3 fwd, rt, up;\n"
         "  sbFrame(fwd, rt, up);\n"
         "  vec2  mid = sbMid();\n"
         "  float sbHalf = abs(sbRange.y - sbRange.x) * 0.5;\n"
         "  vec3 o = vec3(mid.x, 0.0, mid.y) + (uv.x * rt + uv.y * up) * sbHalf +\n"
         "           sbEye() * (sbLift * 3.0 + sbHalf * 3.0);\n"
         "  float sbFar = sbLift * 6.0 + sbHalf * 6.0;\n"
         "  float sbStep = sbFar / 160.0;\n"
         // A vertical gap, seen by an orthographic eye, is that gap times how far
         // the view is from looking straight down it.
         "  float sbToPixels = sqrt(max(1.0 - fwd.y * fwd.y, 0.0)) / max(pixel, 1e-9);\n"
         "  float lag1 = 1e9;\n"
         "  float lag2 = 1e9;\n"
         "  nearest = 1e9;\n"
         "  float t = 0.0;\n"
         "  float was = 1.0;\n"
         "  for (int i = 0; i < 160; ++i) {\n"
         "    t += sbStep;\n"
         "    vec3 p = o + fwd * t;\n"
         "    if (abs(p.y) > sbLift * 1.02 && was > 0.0) continue;\n"
         "    float d = p.y - sbHeight(vec2(p.x, p.z));\n"
         "    if (d <= 0.0 && was > 0.0) {\n"
         // Where between the two steps it crossed. Halving eight times is
         // enough to put it inside a pixel -- and it matters because the
         // *colour* is the height at the point that is hit. Taking the step
         // itself banded the surface along the ray, so the bands moved with
         // the camera and it read as though it were coloured by distance.
         "      float lo = t - sbStep;\n"
         "      float hi = t;\n"
         "      for (int k = 0; k < 8; ++k) {\n"
         "        float mid = 0.5 * (lo + hi);\n"
         "        vec3 m = o + fwd * mid;\n"
         "        if (m.y - sbHeight(vec2(m.x, m.z)) > 0.0) lo = mid; else hi = mid;\n"
         "      }\n"
         "      vec3 h = o + fwd * hi;\n"
         "      vec4 sbHit = sbPaint(sbHeight(vec2(h.x, h.z)));\n"
         // An edge has two sides. The rays that just miss a crest are found by
         // how close they passed; the rays that just *clip* it are found by
         // going on a little and seeing them come back out before they got a
         // pixel and a half deep. A hit that is really a hit is that deep in a
         // step, so this costs nothing where there is no edge.
         "      float sbDeepest = 0.0;\n"
         "      for (int j = 0; j < 24 && sbDeepest < 1.5; ++j) {\n"
         "        t += sbStep;\n"
         "        vec3  q = o + fwd * t;\n"
         "        float e = q.y - sbHeight(vec2(q.x, q.z));\n"
         "        if (e > 0.0) { nearest = min(nearest, sbDeepest); break; }\n"
         "        sbDeepest = max(sbDeepest, -e * sbToPixels);\n"
         "      }\n"
         "      return sbHit;\n"
         "    }\n"
         "    nearest = min(nearest, lag2);\n"
         "    lag2 = lag1;\n"
         "    lag1 = d * sbToPixels;\n"
         "    was = d;\n"
         "  }\n"
         "  nearest = min(nearest, min(lag1, lag2));\n"
         "  return vec4(0.0);\n"
         "}\n"
         // Whether a ray hits is yes or no, so a silhouette is a staircase, and
         // multisampling cannot see it: it is not the edge of any geometry. Four
         // rays for every pixel would be four times a 160-step march on every
         // pixel every frame. So one is marched, and only a ray that passed
         // within a pixel and a half of the surface -- the march samples its
         // approach a dozen pixels apart, so what it sees can be a pixel further
         // off than what happened -- or that clipped a crest and came back out.
         // That is what being next to an edge
         // is, a silhouette or a near hill in front of a far one -- is marched
         // again, four times across the pixel, and the four are averaged by how
         // much each covered. That is a line of pixels, not the window.
         "vec4 sbSurface() {\n"
         "  vec2  uv = sbFilm();\n"
         // Taken before any ray is marched: a derivative is only worth anything
         // outside the loops.
         "  vec2  sbPx = vec2(fwidth(uv.x), fwidth(uv.y));\n"
         "  float pixel = sbPx.x * abs(sbRange.y - sbRange.x) * 0.5;\n"
         "  float nearest;\n"
         "  vec4  c = sbSurfaceRay(uv, pixel, nearest);\n"
         "  if (nearest >= 1.5) return c;\n"
         "  vec4  sum = vec4(0.0);\n"
         "  float ignored;\n"
         "  const vec2 sbGrid[4] = vec2[4](vec2(0.375, 0.125), vec2(-0.125, 0.375),\n"
         "                                 vec2(-0.375, -0.125), vec2(0.125, -0.375));\n"
         "  for (int k = 0; k < 4; ++k) {\n"
         "    vec4 s = sbSurfaceRay(uv + sbGrid[k] * sbPx, pixel, ignored);\n"
         "    sum += vec4(s.rgb * s.a, s.a);\n"
         "  }\n"
         "  return sum.a > 0.0 ? vec4(sum.rgb / sum.a, sum.a * 0.25) : vec4(0.0);\n"
         "}\n";
}

// Where each function in the box begins and ends. Reading the code is
// mathline's job, and this is the same question the box asks when it wants to
// know what a `return` is the answer of.
using Span = mathline::FunctionSpan;
using mathline::FindFunctions;
using mathline::SpanAt;

// The signature line with both type names as GLSL spells them. `RToR`
// is the one name that is ours: it says *which* arrow a function is, where GLSL
// only knows the thing returns a float.
std::string GlslSignature(const std::string& raw, const mathline::FunctionHead& head) {
  const std::string returned = mathline::GlslTypeName(head.returnType);
  const std::string taken = mathline::GlslTypeName(head.paramType);
  if (returned == head.returnType && taken == head.paramType) return raw;

  const std::size_t lead = raw.find_first_not_of(" \t");
  if (lead == std::string::npos) return raw;
  const std::size_t open = raw.find('(', lead);
  if (open == std::string::npos) return raw;
  const std::size_t close = raw.find(')', open);
  if (close == std::string::npos) return raw;
  return raw.substr(0, lead) + returned + " " + head.name + "(" + taken + " " + head.paramName +
         ")" + raw.substr(close + 1);
}

// Feeding a function: one sample per pixel, its answer written straight into a
// buffer. It returns before the body, exactly as drawing a function does --
// what a function does is in its definition, and the box's own lines have
// nothing to say about it.
std::string SampleDispatch(const std::vector<Function>& fns, const clifford::Signature& sig) {
  if (fns.empty()) return {};
  std::string out =
      "  if (sbMode == 3) {\n"
      "    finalColor = vec4(0.0);\n"
      "    int sbK = int(gl_FragCoord.x);\n"
      "    if (sbK >= sbFnCount) return;\n";
  for (std::size_t i = 0; i < fns.size(); ++i) {
    const Function&   fn = fns[i];
    const std::string in = mathline::GlslTypeName(fn.inputType);
    const std::string ret = mathline::GlslTypeName(fn.outputType);
    out += "    " + std::string(i == 0 ? "if" : "else if") + " (sbFnIndex == " +
           std::to_string(i) + ") {\n";
    if (fn.inStride > 1) {
      // Every component at once, which is the one spelling both languages take.
      out += "      " + in + " sbArg = " + in + "(";
      for (int k = 0; k < fn.inStride; ++k) {
        out += (k == 0 ? "" : ", ");
        out += "sbFnIn[sbK * " + std::to_string(fn.inStride) + " + " + std::to_string(k) + "]";
      }
      out += ");\n";
    } else {
      out += "      float sbArg = sbFnIn[sbK];\n";
    }
    out += "      " + ret + " sbAns = " + fn.name + "(sbArg);\n";
    if (fn.outStride > 1) {
      const std::vector<std::string> members = clifford::MemberNames(fn.outputType, sig);
      for (std::size_t k = 0; k < members.size(); ++k) {
        out += "      sbFnOut[sbK * " + std::to_string(fn.outStride) + " + " +
               std::to_string(k) + "] = sbAns." + members[k] + ";\n";
      }
    } else {
      out += "      sbFnOut[sbK] = sbAns;\n";
    }
    out += "    }\n";
  }
  out += "    return;\n  }\n";
  return out;
}

// A sampler box: `Samples(float, 24);` and a body that lays samples down.
//
// The same shape as a viz's mesh half, and for the same reason -- a spread of
// points is geometry, and geometry belongs on the GPU. One invocation: these
// are short loops, and laying them down in parallel would only mean sorting
// them again afterwards.
std::string SamplerPrologue(const std::string& type, int count, int stride,
                            const clifford::Signature& sig,
                            const std::vector<std::string>& algebraTypes, int* lines) {
  std::string out =
      "#version 430\n"
      "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n"
      "layout(std430, binding = 1) buffer SbSamples { uint sbSampleN; float sbSampleV[]; };\n";
  out += glslops::BuiltinOverloads();
  if (!algebraTypes.empty()) out += clifford::GlslPrologue(sig, algebraTypes);
  out += "const int sbCount = " + std::to_string(count) + ";\n";
  // A sampler is allowed to be random and a shader has none of its own, so one
  // is provided -- and the same integer always gives the same number, because a
  // canvas that spread itself differently every frame would be a canvas nobody
  // could point at anything on.
  out +=
      "float sbRand(int i) {\n"
      "  uint h = uint(i) * 747796405u + 2891336453u;\n"
      "  h = ((h >> ((h >> 28) + 4u)) ^ h) * 277803737u;\n"
      "  h = (h >> 22) ^ h;\n"
      "  return float(h) / 4294967296.0;\n"
      "}\n";
  // One way of laying a sample down, for the one type this box is about -- only
  // that one, because that is the only type the box has anything to say about.
  //
  // `pick` rather than `sample`, which is what it would have been called by the
  // rule the other two follow (`vertex`, `line`): GLSL keeps `sample` for
  // multisampling and will not have it as a name. `pick` says the same thing
  // and says it as a verb, which is what choosing a value is.
  out += "void pick(" + mathline::GlslTypeName(type) +
         " v) {\n"
         "  uint i = atomicAdd(sbSampleN, 1u);\n"
         "  if (i >= uint(sbCount)) return;\n";
  if (stride > 1) {
    const std::vector<std::string> members = clifford::MemberNames(type, sig);
    for (std::size_t k = 0; k < members.size(); ++k) {
      out += "  sbSampleV[i * " + std::to_string(stride) + "u + " + std::to_string(k) +
             "u] = v." + members[k] + ";\n";
    }
  } else {
    out += "  sbSampleV[i] = v;\n";
  }
  out += "}\nvoid main() {\n";
  *lines = static_cast<int>(std::count(out.begin(), out.end(), '\n'));
  return out;
}

std::string VizShaderEpilogue() {
  return std::string("  gl_Position = mvp * vec4(p, 0.0, 1.0);\n}\n");
}

std::string ShaderPrologue(const std::vector<Variable>& variables,
                           const clifford::Signature& sig,
                           const std::vector<std::string>& algebraTypes,
                           const std::string& functions, const std::string& seed,
                           const std::vector<std::string>& functionNames,
                           const std::vector<Function>& defined, bool complex, int* lines,
                           int* functionsAt) {
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
      "uniform vec3 sbOrbit;\n"     // where the eye is: yaw, pitch, how far off
      "uniform float sbLift;\n"     // how far a surface is allowed to rise
      "layout(std430, binding = 0) buffer SbTrace { float sbTrace[]; };\n";
  // Feeding a function: the spread of inputs goes in, the answers come out. Two
  // buffers rather than a pixel each, because an answer is a whole multivector
  // as readily as it is one number.
  if (!defined.empty()) {
    out +=
        "uniform int sbFnIndex;\n"
        "uniform int sbFnCount;\n"
        "layout(std430, binding = 3) readonly buffer SbFnIn { float sbFnIn[]; };\n"
        "layout(std430, binding = 4) buffer SbFnOut { float sbFnOut[]; };\n";
  }
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
  // The plane with a multiplication on it, written out only for a box that says
  // so. Its products answer to the names the operator rewrite produces, so
  // `z * w` in a box is `gp(z, w)` here.
  if (complex) out += types::complexnum::GlslPrologue();
  out += PaintSource(defined);

  // **A box's variables are globals.** GLSL has nowhere to put a nested
  // function, so a function the user writes is hoisted above main -- and a
  // hoisted function cannot see a local. `h(x) = f(x) + a w(x)` is exactly what
  // this is for: a function whose shape a knob reshapes, which is the whole
  // reason a function is GLSL at all.
  for (const Variable& variable : variables) {
    if (variable.kind == Kind::Real) {
      out += "float " + variable.name + ";\n";
    } else if (variable.kind == Kind::Multivector) {
      out += variable.type + " " + variable.name + ";\n";
    }
  }

  // The user's own functions, hoisted out of main because GLSL has nowhere else
  // to put them. One line per line of the box, so the block keeps the box's
  // shape even though most of its lines are blank. Where it starts is *reported*
  // rather than worked back out from the end: everything after it grows as the
  // language does, and a derived offset drifts silently.
  *functionsAt = static_cast<int>(std::count(out.begin(), out.end(), '\n'));
  out += functions;
  const std::string surface = SurfaceDispatch(defined);
  const std::string volume = VolumeDispatch(defined);
  if (!surface.empty() || !volume.empty()) out += EyeSource();
  out += surface;
  out += volume;

  // Which of the box's functions a field or a curve is drawing.
  out += "float sbEval(float x) {\n";
  for (std::size_t i = 0; i < functionNames.size(); ++i) {
    out += "  " + std::string(i == 0 ? "if" : "else if") + " (sbIndex == " + std::to_string(i) +
           ") return " + functionNames[i] + "(x);\n";
  }
  out +=
      "  return 0.0;\n"
      "}\n"
      "void main() {\n";
  // What a drawing is made from: the value every variable was *given*, before
  // the body has had a chance to assign over it. Every way of drawing a function
  // returns before the body, so this is what they all have to go on -- and it
  // is the right thing to go on, since what a knob moves is exactly one of
  // these literals. It comes first, before any of them: it used to sit below
  // the picture, the surface and the volume, which had been added above it, so
  // those three drew with every variable at nought and their knobs did nothing.
  out += seed;
  // A picture is drawn from the definitions alone, like every other way of
  // drawing a function, so it comes before the body and returns.
  out += PictureDispatch(defined);
  if (!surface.empty()) {
    out +=
        "  if (sbMode == 2 && sbShape == 3) {\n"
        "    vec4 c = sbSurface();\n"
        "    if (c.a <= 0.0) discard;\n"
        "    finalColor = c;\n"
        "    return;\n"
        "  }\n";
  }
  if (!volume.empty()) {
    out +=
        "  if (sbMode == 2 && sbShape == 4) {\n"
        "    vec4 c = sbVolume();\n"
        "    if (c.a <= 0.002) discard;\n"
        // Already multiplied through by how much of the ray each step covered,
        // so it goes down as it is rather than being multiplied again.
        "    finalColor = vec4(c.rgb / max(c.a, 1e-4), c.a);\n"
        "    return;\n"
        "  }\n";
  }
  out +=
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
      // By how much of the pixel the band covers, rather than in or out. In or
      // out is a staircase, and multisampling cannot help: the edge is decided
      // in here, not by the edge of any geometry. `fwidth` is how far the value
      // moves across one pixel, which turns a distance in value units into one
      // in pixels -- both axes are at one scale, so either direction will do.
      "    float sbGap = abs(here - sbOut) / sqrt(1.0 + slope * slope);\n"
      "    float sbCover = clamp((sbThick - sbGap) / max(fwidth(here), 1e-6) + 0.5, 0.0, 1.0);\n"
      "    if (sbCover <= 0.0) discard;\n"
      "    finalColor = vec4(fragColor.rgb, fragColor.a * sbCover);\n"
      "    return;\n"
      "  }\n"
      // Reporting asks about one point rather than the one being drawn, and so
      // does feeding a function: inside a fragment shader a variable is a field
      // rather than a number, so both have to say *where*.
      "  vec2 ";
  out += kShaderInput;
  out += " = (sbMode == 1 || sbMode == 3) ? sbProbe : sbVarying;\n";
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

std::string ProbeEpilogue(const std::vector<std::string>& probed,
                          const std::vector<Function>& defined,
                          const clifford::Signature& sig) {
  // Feeding a function comes *after* the body, unlike drawing one. It runs once
  // per change over a couple of hundred fragments rather than per pixel per
  // frame, so it can afford to be exact -- which means a function may read
  // anything the box computed, not only what its variables were declared with.
  std::string out = SampleDispatch(defined, sig);
  out +=
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

}  // namespace

void Program::Rebuild(const std::vector<std::string>& lines) {
  // Remember what the knobs were set to, so an edit does not reset them.
  std::unordered_map<std::string, float> previous;
  for (const Variable& variable : variables_) {
    if (variable.free) previous.emplace(variable.name, variable.value);
  }

  // Which algebra this box works in, and which of its types it mentions. Both
  // are read off the box itself: `Cl(3,0,1);` on a line of its own changes
  // the signature, and everything else defaults to 3D projective geometric
  // algebra, which is what these tools are mostly for.
  clifford::Signature signature = clifford::kDefaultSignature;
  std::string         joined;
  // And what, if anything, this box draws. `Viz(Real);` makes it a viz: it says
  // how a value of that type is shown rather than declaring one, so it is
  // assembled entirely differently from here on.
  vizType_.clear();
  samplerType_.clear();
  samplerCount_ = 0;
  samplerStride_ = 1;
  glslSample_.clear();
  tracedLines_ = static_cast<int>(lines.size());
  for (const std::string& raw : lines) {
    clifford::ParseAlgebra(raw, &signature);
    const mathline::Line sigil = mathline::Parse(raw);
    if (sigil.kind == mathline::Kind::Viz) vizType_ = sigil.name;
    // And what, if anything, it spreads out. `Samples(float, 24);` makes a box
    // a sampler: it says how a type's space is laid out for a function to be
    // fed from, rather than declaring a value of its own.
    if (sigil.kind == mathline::Kind::Samples) {
      samplerType_ = sigil.name;
      samplerCount_ = sigil.count;
    }
    joined += raw;
    joined += '\n';
  }
  const std::vector<std::string> algebraTypes = clifford::TypesMentioned(joined, signature);

  // Every function the box defines, and where each one begins and ends. Found
  // before anything is assembled, because a function's lines are hoisted out of
  // the body wholesale and the loop below has to know which those are.
  std::vector<Span> spans = FindFunctions(lines, signature);
  // A prototype whose function the box goes on to give a body is not a function
  // of its own: it is a forward declaration, and the body is the function. So
  // its span goes, and its line is left to be the declaration GLSL already
  // understands it as. Only a prototype nobody defined gets a body written.
  std::erase_if(spans, [&](const Span& prototype) {
    if (prototype.head.body) return false;
    for (const Span& other : spans) {
      if (other.head.body && other.head.name == prototype.head.name) return true;
    }
    return false;
  });
  functions_.clear();
  for (const Span& span : spans) {
    Function fn;
    fn.name = span.head.name;
    fn.inputType = span.head.paramType;
    fn.outputType = span.head.returnType;
    fn.firstLine = span.first;
    fn.lastLine = span.last;
    fn.inStride = ComponentCount(fn.inputType, signature);
    fn.outStride = ComponentCount(fn.outputType, signature);
    fn.in = SpaceOf(fn.inputType, signature);
    fn.out = SpaceOf(fn.outputType, signature);
    // Reals to reals is already a curve on a plot and a field on an axis. Those
    // special cases win where they apply; everything else is sampled.
    // Reals to reals is a curve and a field; a function of two variables is a
    // picture. Both are drawn from the definition at every point of the input,
    // so neither is sampled. Sampling is what is left for the ones with no
    // picture to be.
    // Anything with a window of its own is drawn in it, so it is not also fed:
    // `IsDrawn` rather than `IsPicture`, which left out the volume and marked a
    // function of three variables as sampled. Nothing came of it -- there is no
    // spread of a vec3 to feed it with, so the feed bailed -- but the shader
    // was carrying the code to do it.
    const bool curve = fn.in == Space::R1 && fn.out == Space::R1;
    fn.sampled = !curve && !fn.IsDrawn();
    functions_.push_back(fn);
  }

  // Whether this box is an ordinary one -- neither a viz nor a sampler. Both of
  // those are assembled around a prologue of their own, with no globals in it
  // and no variables to have them, so their declarations stay the locals they
  // were written as. Known here rather than at the end because the loop below
  // has to emit one or the other as it goes.
  const bool ordinary = vizType_.empty() && samplerType_.empty();

  std::vector<Variable> variables;
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
      shaderBody += "\n";
      shaderFunctions += "\n";
      continue;
    }

    const mathline::Line line = mathline::Parse(raw, signature);
    // So does the line that says what this box draws, and the line that says
    // how its space is spread out.
    if (line.kind == mathline::Kind::Viz || line.kind == mathline::Kind::Samples) {
      shaderBody += "\n";
      shaderFunctions += "\n";
      continue;
    }
    // A prototype that is not a span of its own is one the box defines below,
    // and it is hoisted as written -- a forward declaration is exactly what
    // lets a function above that body call it.
    if (line.kind == mathline::Kind::Prototype && SpanAt(spans, thisLine) < 0) {
      shaderFunctions += raw + "\n";
      shaderBody += "\n";
      continue;
    }

    // Inside a function definition. GLSL has nowhere to put a nested function,
    // so the whole span is hoisted above main -- one line for one line, so both
    // halves keep the box's shape and a line number is still a line number.
    if (const int span = SpanAt(spans, thisLine); span >= 0) {
      const Span& found = spans[static_cast<std::size_t>(span)];
      if (!found.head.body) {
        // Nobody said what this function does, so the app does -- the same move
        // as injecting `= 0.2385` into `float a;`, made for a function.
        shaderFunctions +=
            mathline::GlslTypeName(found.head.returnType) + " " + found.head.name + "(" +
            mathline::GlslTypeName(found.head.paramType) + " " + found.head.paramName +
            ") { return " +
            types::realfunction::ExampleFunctionBody(found.head.returnType, found.head.paramType,
                                                     found.head.paramName, found.head.name,
                                                     signature) +
            "; }\n";
      } else {
        shaderFunctions += (thisLine == found.first ? GlslSignature(raw, found.head) : raw) + "\n";
      }
      shaderBody += "\n";

      // A function of reals to reals is a value as far as the windows are
      // concerned: it gets a plot and a field, which are its two special cases.
      // Its *type* is RToR however it was spelled, because that is what
      // a viz for one has to claim. Only that one shape: `sbEval` dispatches
      // over these by calling them with a float and returning a float, so a
      // picture in the list is a picture called with the wrong thing.
      const Function& made = functions_[static_cast<std::size_t>(span)];
      if (thisLine == found.first && made.in == Space::R1 && made.out == Space::R1) {
        Variable variable;
        variable.declaredLine = thisLine;
        variable.name = found.head.name;
        variable.type = types::realfunction::kName;
        variable.latexName = mathline::NameToLatex(found.head.name);
        variable.kind = Kind::Function;
        variable.free = false;
        variables.push_back(variable);
      }
      continue;
    }

    if (line.kind == mathline::Kind::Declaration && !line.hasInitialiser) {
      // The abstract declaration the user wrote; the initialiser is ours.
      Variable variable;
      variable.declaredLine = thisLine;
      variable.name = line.name;
      variable.type = line.type;
      variable.latexName = mathline::NameToLatex(line.name);
      variable.kind =
          mathline::IsMultivectorType(line.type, signature) ? Kind::Multivector : Kind::Real;
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
        const std::string chosen = clifford::ExampleValue(line.type, line.name, signature);
        // The variable itself is a global, declared in the prologue so that the
        // box's functions can see it. This line only gives it its value.
        shaderBody += "  " + (ordinary ? "" : line.type + " ") + line.name + " = " + chosen +
                      ";\n";
        shaderFunctions += "\n";
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

      // Not a uniform itself: a uniform cannot be assigned over, and a variable
      // in a box can. And not a local either -- it is a global, so that the
      // box's own functions can see it.
      shaderBody +=
          "  " + std::string(ordinary ? "" : "float ") + line.name + " = " + injected.uniform +
          ";\n";
      ++freeIndex;
      continue;
    }

    if (line.kind == mathline::Kind::Declaration && line.hasInitialiser) {
      Variable variable;
      variable.declaredLine = thisLine;
      variable.name = line.name;
      variable.type = line.type;
      variable.latexName = mathline::NameToLatex(line.name);
      variable.kind =
          mathline::IsMultivectorType(line.type, signature) ? Kind::Multivector : Kind::Real;
      variable.free = false;
      variables.push_back(variable);
      addSlots(variable, thisLine);

      // Its value is computed here; the variable itself is a global.
      if (ordinary && (variable.kind == Kind::Real || variable.kind == Kind::Multivector)) {
        shaderBody += "  " + WithoutType(raw, line.type) + "\n";
        shaderFunctions += "\n";
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

        shaderBody += "  " + variable.name + " = " + literal.uniform + ";\n";
        shaderFunctions += "\n";
        handled = true;
        break;
      }
      if (handled) continue;
    }

    // Anything else is passed through verbatim: it is either GLSL already, or
    // it is the user's mistake to see reported.
    shaderBody += "  " + raw + "\n";
    shaderFunctions += "\n";
  }

  // A sampler box has no variables and wants none either: what it produces is a
  // spread of points for somebody else's function to be fed from. It is the
  // same shape as a viz's mesh half -- a compute shader writing into a buffer
  // through an atomic counter -- because a spread of points is geometry.
  if (!samplerType_.empty()) {
    variables_.clear();
    probed_.clear();
    functions_.clear();
    samplerStride_ = std::min(ComponentCount(samplerType_, signature), kMaxStride);
    samplerCount_ = std::clamp(samplerCount_, 1, kMaxSamples);

    std::string sampleBody = shaderBody;
    glslops::Rewrite(shaderBody, &sampleBody, nullptr);

    int head = 0;
    glslSample_ = SamplerPrologue(samplerType_, samplerCount_, samplerStride_, signature,
                                  algebraTypes, &head) +
                  sampleBody + "}\n";
    glslLines_ = {head, head, static_cast<int>(lines.size())};
    glsl_.clear();
    glslMesh_.clear();
    state_ = State::Shading;
    error_ = {};
    return;
  }

  // A viz box has no variables and wants none: what it produces is a mesh, or
  // the shader that places one. Both halves are assembled from the same lines.
  if (!vizType_.empty()) {
    variables_.clear();
    probed_.clear();

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

    // Shading means "the host should compile this on the thread that owns the
    // graphics context", which is the only way a box is built now.
    state_ = State::Shading;
    error_ = {};
    return;
  }

  // A box of nothing but function definitions is still a box worth building:
  // it defines machines, and a machine can be fed and drawn even though it
  // declares no value of its own.
  if (variables.empty() && functions_.empty()) {
    variables_.clear();
    state_ = State::Empty;
    // A box with nothing in it should be holding nothing.
    glsl_.clear();
    probed_.clear();
    return;
  }

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

  // The value each variable is *given* where it is declared, set before the
  // modes that return early, so a curve or a field has the numbers its
  // functions were written against.
  std::string seed;
  for (const Variable& variable : variables) {
    if (variable.kind == Kind::Real && !variable.literals.empty()) {
      seed += "  " + variable.name + " = " + variable.literals.front().uniform + ";\n";
    }
  }

  int prologueLines = 0;
  int functionsAt = 0;
  const std::string head =
      ShaderPrologue(variables, signature, algebraTypes, functions, seed, functionNames_,
                     functions_, joined.find(types::complexnum::kName) != std::string::npos,
                     &prologueLines, &functionsAt);
  // The box's lines appear twice: hoisted above main, and again inside it.
  glslLines_ = {functionsAt, prologueLines, static_cast<int>(lines.size())};
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

  glsl_ = head + traced + ProbeEpilogue(probed_, functions_, signature);

  variables_ = std::move(variables);
  // There is one language. A box is GLSL, and the host compiles it on the
  // thread that owns the graphics context -- which is what Shading means.
  state_ = State::Shading;
  error_ = {};
}

int Program::FunctionAt(int line) const {
  if (functions_.empty()) return -1;
  // Out of range means the end -- which is where a box nobody has touched is
  // read from, and for a box that is nothing but a definition that is inside it.
  const int at = (line < 0) ? std::max(0, tracedLines_ - 1) : line;
  for (std::size_t i = 0; i < functions_.size(); ++i) {
    if (functions_[i].Spans(at)) return static_cast<int>(i);
  }
  return -1;
}

void Program::ShaderCompiled(bool ok, const std::string& log) {
  if (state_ != State::Shading) return;
  if (ok) {
    state_ = State::ShaderReady;
    error_ = {};
    return;
  }

  state_ = State::Failed;
  error_ = compiler::ParseGlslLog(log, glslLines_);
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
