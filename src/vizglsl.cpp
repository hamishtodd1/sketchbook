#include "vizglsl.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>

#include "glslops.h"
#include "mathline.h"
#include "types/Color.h"
#include "types/Complex.h"

namespace sb::vizglsl {

namespace {

// The windows that ask for a viz, as a viz box names them.
const char* const kWindows[] = {"Space",   "Spacetime", "Axis",   "Plot",    "Picture",
                                "Surface", "Volume",    "Picker", "Cube", "Cylinder"};

// What each standard name is, and which stage it belongs to.
enum class Stage { Compute, Vertex, Fragment };

struct Standard {
  std::string base;    // mesh, vert, frag, position, grab, drag
  int         piece = 0;
  Stage       stage = Stage::Compute;
};

// `mesh`, `vert3`, `grab`... or nothing, for a helper. `bad` is set for a name
// that is standard but numbered out of range.
bool StandardName(const std::string& name, Standard* out, std::string* bad) {
  static const char* const kBases[] = {"mesh", "vert", "frag", "position", "grab", "drag"};
  for (const char* base : kBases) {
    const std::size_t n = std::char_traits<char>::length(base);
    if (name.compare(0, n, base) != 0) continue;
    const std::string rest = name.substr(n);
    if (!rest.empty() && !std::all_of(rest.begin(), rest.end(),
                                      [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
      continue;
    }
    int piece = 0;
    if (!rest.empty()) {
      const int number = std::stoi(rest);
      const std::string b = base;
      if (b == "grab" || b == "drag") {
        *bad = "there is one `" + b + "` per viz, not one per piece";
        return false;
      }
      if (number > kMaxPieces) {
        *bad = "a viz has at most " + std::to_string(kMaxPieces) + " pieces, so there is no `" +
               name + "`";
        return false;
      }
      if (number < 2) {
        *bad = "the first piece has no number: `" + b + "`, then `" + b + "2`";
        return false;
      }
      piece = number - 1;
    }
    out->base = base;
    out->piece = piece;
    const std::string b = base;
    out->stage = (b == "vert") ? Stage::Vertex : (b == "frag") ? Stage::Fragment : Stage::Compute;
    return true;
  }
  return false;
}

// A line with its comment taken off, which is all brace-counting needs.
std::string_view Code(std::string_view line) {
  const std::size_t comment = line.find("//");
  return comment == std::string_view::npos ? line : line.substr(0, comment);
}

bool IdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IdentPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// `vec3 position(Tri v) {` -- a type, a name, and a bracket. The name, or empty.
std::string HeadName(std::string_view line) {
  std::size_t at = line.find_first_not_of(" \t");
  if (at == std::string_view::npos || !IdentStart(line[at])) return {};
  while (at < line.size() && IdentPart(line[at])) ++at;   // the type
  const std::size_t gap = at;
  while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
  if (at == gap || at >= line.size() || !IdentStart(line[at])) return {};
  const std::size_t begin = at;
  while (at < line.size() && IdentPart(line[at])) ++at;
  const std::string name(line.substr(begin, at - begin));
  while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
  if (at >= line.size() || line[at] != '(') return {};
  return name;
}

struct Span {
  std::string name;
  int         first = 0;
  int         last = 0;
};

// Every function the window defines, first line to closing brace.
std::vector<Span> Functions(const std::vector<std::string>& lines) {
  std::vector<Span> out;
  int  depth = 0;
  bool open = false;
  bool opened = false;
  Span current;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view code = Code(lines[i]);
    if (depth == 0 && !open) {
      const std::string name = HeadName(code);
      if (!name.empty()) {
        open = true;
        opened = false;
        current = {name, static_cast<int>(i), static_cast<int>(i)};
      }
    }
    for (char c : code) {
      if (c == '{') { ++depth; opened = true; }
      if (c == '}') depth = std::max(0, depth - 1);
    }
    if (!open) continue;
    if (opened && depth == 0) {
      current.last = static_cast<int>(i);
      out.push_back(current);
      open = false;
    } else if (!opened && code.find(';') != std::string_view::npos) {
      open = false;   // a prototype, which is not a definition
    }
  }
  return out;
}

int CountLines(const std::string& text) {
  return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

// How many numbers one value of the type is, and the names of those numbers as
// GLSL reaches them. A real is itself.
std::vector<std::string> Parts(const std::string& type, const clifford::Signature& sig) {
  if (type == "float") return {""};
  if (mathline::IsColorType(type)) return {"x", "y", "z"};
  if (mathline::IsComplexType(type)) return types::complexnum::MemberNames();
  return clifford::MemberNames(type, sig);
}

}  // namespace

bool KnownWindow(const std::string& dw) {
  return std::find_if(std::begin(kWindows), std::end(kWindows),
                      [&](const char* w) { return dw == w; }) != std::end(kWindows);
}

std::string KnownWindows() {
  std::string out;
  for (std::size_t i = 0; i < std::size(kWindows); ++i) {
    if (i > 0) out += (i + 1 == std::size(kWindows)) ? " and " : ", ";
    out += kWindows[i];
  }
  return out;
}

// ------------------------------------------------------------- functions

namespace {

// R, R2, R3 and C, as GLSL spells them.
std::string GlslOfSpace(std::string_view space) {
  if (space == "R") return "float";
  if (space == "R2") return "vec2";
  if (space == "R3") return "vec3";
  if (space == "C") return "Complex";
  return {};
}

std::string SpaceOfGlsl(const std::string& type) {
  if (type == "float") return "R";
  if (type == "vec2") return "R2";
  if (type == "vec3" || mathline::IsColorType(type)) return "R3";
  if (mathline::IsComplexType(type)) return "C";
  return {};
}

std::string ZeroOf(const std::string& glsl) {
  if (glsl == "float") return "0.0";
  if (glsl == "Complex") return "Complex(0.0, 0.0)";
  return glsl + "(0.0)";
}

// Every identifier in `text` found in `to` becomes what it maps to -- but not
// in a comment, and not after a dot, where it is a member rather than a name.
std::string Rename(const std::string& text, const std::map<std::string, std::string>& to) {
  std::string out;
  out.reserve(text.size() + 64);
  std::size_t i = 0;
  char        before = ' ';
  while (i < text.size()) {
    if (text.compare(i, 2, "//") == 0) {
      const std::size_t end = text.find('\n', i);
      const std::size_t stop = (end == std::string::npos) ? text.size() : end;
      out.append(text, i, stop - i);
      i = stop;
      continue;
    }
    if (IdentStart(text[i])) {
      std::size_t j = i;
      while (j < text.size() && IdentPart(text[j])) ++j;
      const std::string word = text.substr(i, j - i);
      const auto found = to.find(word);
      out += (found != to.end() && before != '.') ? found->second : word;
      before = 'a';
      i = j;
      continue;
    }
    if (text[i] != ' ' && text[i] != '\t') before = text[i];
    out += text[i++];
  }
  return out;
}

// A global a window declares at the top of it, `const vec3 green = ...;`, which
// has to be renamed along with its functions for two vizes to share a box.
std::string GlobalName(std::string_view line) {
  std::size_t at = line.find_first_not_of(" \t");
  if (at == std::string_view::npos || line.compare(at, 6, "const ") != 0) return {};
  at += 6;
  while (at < line.size() && line[at] == ' ') ++at;
  while (at < line.size() && IdentPart(line[at])) ++at;   // the type
  while (at < line.size() && line[at] == ' ') ++at;
  const std::size_t begin = at;
  while (at < line.size() && IdentPart(line[at])) ++at;
  return std::string(line.substr(begin, at - begin));
}

Sources AssembleFunction(const std::vector<std::string>& lines, const std::string& type,
                         const std::string& dw) {
  Sources out;
  out.function = true;
  std::string in, ret;
  FunctionType(type, &in, &ret);
  const std::vector<std::string> windows = FunctionWindows(type);
  if (std::find(windows.begin(), windows.end(), dw) == windows.end()) {
    out.error = windows.empty() ? "no window draws a function `" + type + "`"
                                : "a function `" + type + "` is drawn in " + windows.front() +
                                      (windows.size() > 1 ? " or " + windows.back() : "") +
                                      ", not in " + dw;
    return out;
  }

  // Which of its names are standard, and every name it defines -- all of which
  // are renamed, so two vizes with a helper of the same name can share a box.
  std::map<std::string, std::string> to;
  bool frag = false;
  for (const Span& span : Functions(lines)) {
    if (span.name == "frag") frag = true;
    else if (span.name == "grab") out.grab = true;
    else if (span.name == "mesh" || span.name == "vert" || span.name == "position" ||
             span.name == "drag") {
      out.error = "a viz of a function is drawn in its fragments: it has `frag`, and `grab`, "
                  "and no `" + span.name + "`";
      out.errorLine = span.first;
      return out;
    }
    to[span.name] = Spliced(span.name, type, dw);
  }
  for (const std::string& line : lines) {
    const std::string global = GlobalName(line);
    if (!global.empty()) to[global] = Spliced(global, type, dw);
  }
  if (!frag) {
    out.error = "a viz of a function says how its fragments look, in `vec4 frag(vec2 at)`";
    return out;
  }
  to["value"] = Spliced("value", type, dw);

  std::string joined;
  for (const std::string& line : lines) {
    const bool sigil = mathline::Parse(line).kind == mathline::Kind::Viz ||
                       clifford::ParseAlgebra(line, nullptr);
    joined += (sigil ? std::string() : line) + "\n";
  }
  std::string rewritten;
  if (!glslops::Rewrite(joined, &rewritten, nullptr) || rewritten.empty()) rewritten = joined;
  out.spliced = Rename(rewritten, to);

  // Standing alone, against a function that is always nought: enough to find
  // every mistake the window can make by itself, on its own lines.
  const bool complex = in == "Complex" || ret == "Complex" ||
                       joined.find(types::complexnum::kName) != std::string::npos;
  const bool colour = joined.find(types::color::kName) != std::string::npos;
  std::string head =
      "#version 430\n"
      "in vec2 sbVarying;\n"
      "in vec4 fragColor;\n"
      "out vec4 finalColor;\n" +
      FunctionUniforms() + glslops::BuiltinOverloads();
  if (complex) head += types::complexnum::GlslPrologue();
  if (colour) head += types::color::GlslPrologue();
  head += ret + " " + Spliced("value", type, dw) + "(" + in + " x) { return " + ZeroOf(ret) +
          "; }\n";
  head += FunctionHelpers(ret == "float", ret == "vec3", ret == "Complex", dw == "Surface",
                          dw == "Volume");
  head += "vec4 sbTint;\n";
  out.bodyAt = CountLines(head);
  out.standalone = head + out.spliced +
                   "void main() {\n"
                   "  sbTint = fragColor;\n"
                   "  vec2 sbHere = " + AtOf(dw) + ";\n"
                   "  finalColor = " + Spliced("frag", type, dw) + "(sbHere);\n" +
                   (out.grab ? "  finalColor.a += " + Spliced("grab", type, dw) + "(sbHere) * 0.0;\n"
                             : std::string()) +
                   "}\n";
  return out;
}

}  // namespace

bool FunctionType(const std::string& type, std::string* in, std::string* out) {
  const std::size_t to = type.find("To");
  if (to == std::string::npos || to == 0) return false;
  const std::string a = GlslOfSpace(std::string_view(type).substr(0, to));
  const std::string b = GlslOfSpace(std::string_view(type).substr(to + 2));
  if (a.empty() || b.empty()) return false;
  if (in != nullptr) *in = a;
  if (out != nullptr) *out = b;
  return true;
}

std::string FunctionTypeOf(const std::string& in, const std::string& out) {
  const std::string a = SpaceOfGlsl(in);
  const std::string b = SpaceOfGlsl(out);
  return (a.empty() || b.empty()) ? std::string() : a + "To" + b;
}

std::vector<std::string> FunctionWindows(const std::string& type) {
  if (type == "RToR") return {"Plot", "Axis"};
  if (type == "R2ToR") return {"Surface"};
  if (type == "R2ToR3" || type == "R2ToC" || type == "CToR" || type == "CToR3" ||
      type == "CToC") {
    return {"Picture"};
  }
  if (type == "R3ToR" || type == "R3ToC") return {"Volume"};
  return {};
}

int ShapeOf(const std::string& dw) {
  if (dw == "Axis") return 0;
  if (dw == "Plot") return 1;
  if (dw == "Picture") return 2;
  if (dw == "Surface") return 3;
  if (dw == "Volume") return 4;
  return -1;
}

std::string Spliced(const std::string& name, const std::string& type, const std::string& dw) {
  std::string base = name;
  if (name == "frag") base = "sbFrag";
  else if (name == "grab") base = "sbGrab";
  else if (name == "value") base = "sbValue";
  return base + "_" + type + "_" + dw;
}

std::string AtOf(const std::string& dw) {
  if (dw == "Axis") return "vec2(mix(sbRange.x, sbRange.y, sbVarying.x), 0.0)";
  if (dw == "Plot" || dw == "Picture") {
    return "vec2(mix(sbRange.x, sbRange.y, sbVarying.x), mix(sbVertical.x, sbVertical.y, "
           "sbVarying.y))";
  }
  return "sbFilm()";
}

std::string FunctionUniforms() {
  return "uniform vec2 sbRange;\n"
         "uniform vec2 sbVertical;\n"
         "uniform int sbIndex;\n"
         "uniform int sbShape;\n"
         "uniform float sbThick;\n"
         "uniform vec3 sbOrbit;\n"
         "uniform float sbLift;\n"
         "uniform float sbPixel;\n"
         "uniform vec2 sbAt;\n";
}

std::string FunctionHelpers(bool paintReal, bool paintColour, bool paintComplex, bool surface,
                            bool volume) {
  std::string out;
  // How a value is coloured. A real is viridis, signed: the same map a function
  // of one variable gets along a number line, so the colour means the same
  // number whichever window it is read in.
  if (paintReal) {
    out +=
        "vec4 sbPaint(float v) {\n"
        "  float t = clamp(v, -1.0, 1.0);\n"
        "  vec3 green  = vec3(109.0 / 255.0, 205.0 / 255.0,  89.0 / 255.0);\n"
        "  vec3 yellow = vec3(253.0 / 255.0, 231.0 / 255.0,  37.0 / 255.0);\n"
        "  return vec4(mix(vec3(0.0), t >= 0.0 ? green : yellow, abs(t)), 1.0);\n"
        "}\n"
        "vec3 sbColourOf(float v) {\n"
        "  vec3 green  = vec3(109.0 / 255.0, 205.0 / 255.0,  89.0 / 255.0);\n"
        "  vec3 yellow = vec3(253.0 / 255.0, 231.0 / 255.0,  37.0 / 255.0);\n"
        "  return v >= 0.0 ? green : yellow;\n"
        "}\n"
        "float sbThickness(float v) { return clamp(abs(v), 0.0, 1.0); }\n";
  }
  // A vec3 *is* a colour.
  if (paintColour) out += "vec4 sbPaint(vec3 c) { return vec4(c, 1.0); }\n";
  // Domain colouring: the argument is the hue and the size the lightness, black
  // at a zero and white as it runs away.
  if (paintComplex) {
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
        "vec3 sbColourOf(Complex z) { return sbHue(carg(z) / 6.2831853 + 0.5); }\n"
        "float sbThickness(Complex z) { float m = cabs(z); return m / (1.0 + m); }\n";
  }
  if (surface || volume) {
    // Where the eye is and which way is up, and the patch being looked at.
    out +=
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
        "}\n"
        "float sbHalfSpan() { return abs(sbRange.y - sbRange.x) * 0.5; }\n";
  }
  if (surface) {
    // **Orthographic**: a surface is a graph, and a graph must draw the same
    // quantity the same size wherever it stands.
    out +=
        "void sbSurfaceCamera(vec2 film, out vec3 o, out vec3 d) {\n"
        "  vec3 fwd, rt, up;\n"
        "  sbFrame(fwd, rt, up);\n"
        "  vec2 mid = sbMid();\n"
        "  float span = sbHalfSpan();\n"
        "  o = vec3(mid.x, 0.0, mid.y) + (film.x * rt + film.y * up) * span +\n"
        "      sbEye() * (sbLift * 3.0 + span * 3.0);\n"
        "  d = fwd;\n"
        "}\n"
        "float sbHeightAt(vec2 q) { return clamp(" + Spliced("value", "R2ToR", "Surface") +
        "(q), -sbLift, sbLift); }\n"
        // Where a ray first meets the surface, or -1. `nearest` comes back as how
        // close, in pixels, it passed to the surface *before* the approach it
        // landed on -- or, for a ray that clipped a crest and came back out, how
        // deep it got -- which is what says the ray is next to an edge.
        "float sbMarch(vec3 o, vec3 fwd, float pixel, out float nearest) {\n"
        "  float sbFar = sbLift * 6.0 + sbHalfSpan() * 6.0;\n"
        "  float sbStep = sbFar / 160.0;\n"
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
        "    float d = p.y - sbHeightAt(vec2(p.x, p.z));\n"
        "    if (d <= 0.0 && was > 0.0) {\n"
        // Where between the two steps it crossed, by halving: the colour is the
        // height at the point hit, so this is a question of the colour being
        // right, not only of smoothness.
        "      float lo = t - sbStep;\n"
        "      float hi = t;\n"
        "      for (int k = 0; k < 8; ++k) {\n"
        "        float mid = 0.5 * (lo + hi);\n"
        "        vec3 m = o + fwd * mid;\n"
        "        if (m.y - sbHeightAt(vec2(m.x, m.z)) > 0.0) lo = mid; else hi = mid;\n"
        "      }\n"
        "      float deepest = 0.0;\n"
        "      for (int j = 0; j < 24 && deepest < 1.5; ++j) {\n"
        "        t += sbStep;\n"
        "        vec3  q = o + fwd * t;\n"
        "        float e = q.y - sbHeightAt(vec2(q.x, q.z));\n"
        "        if (e > 0.0) { nearest = min(nearest, deepest); break; }\n"
        "        deepest = max(deepest, -e * sbToPixels);\n"
        "      }\n"
        "      return hi;\n"
        "    }\n"
        "    nearest = min(nearest, lag2);\n"
        "    lag2 = lag1;\n"
        "    lag1 = d * sbToPixels;\n"
        "    was = d;\n"
        "  }\n"
        "  nearest = min(nearest, min(lag1, lag2));\n"
        "  return -1.0;\n"
        "}\n";
  }
  if (volume) {
    // **Perspective**: a volume is a place you are looking into, and there the
    // near things being bigger is exactly what says they are near.
    out +=
        "vec3 sbHub() { vec2 mid = sbMid(); return vec3(mid.x, mid.y, 0.0); }\n"
        "void sbVolumeCamera(vec2 film, out vec3 o, out vec3 d) {\n"
        "  vec3 fwd, rt, up;\n"
        "  sbFrame(fwd, rt, up);\n"
        "  o = sbHub() + sbEye() * sbOrbit.z * sbHalfSpan();\n"
        "  d = normalize(fwd + (film.x * rt + film.y * up) * tan(0.4));\n"
        "}\n"
        // Where a ray is inside the cube, as the distances along it it goes in
        // and comes out at -- the usual slab test. Empty if it misses.
        "vec2 sbClip(vec3 o, vec3 d) {\n"
        "  vec3 inv = 1.0 / d;\n"
        "  vec3 t0 = (sbHub() - vec3(sbHalfSpan()) - o) * inv;\n"
        "  vec3 t1 = (sbHub() + vec3(sbHalfSpan()) - o) * inv;\n"
        "  vec3 tn = min(t0, t1);\n"
        "  vec3 tf = max(t0, t1);\n"
        "  return vec2(max(max(max(tn.x, tn.y), tn.z), 0.0), min(min(tf.x, tf.y), tf.z));\n"
        "}\n";
  }
  return out;
}

Sources Assemble(const std::vector<std::string>& lines, const std::string& type,
                 const std::string& dw, const clifford::Signature& sig,
                 const std::vector<std::string>& algebraTypes) {
  if (FunctionType(type, nullptr, nullptr)) return AssembleFunction(lines, type, dw);
  Sources out;
  const std::vector<std::string> parts = Parts(type, sig);
  if (parts.empty()) {
    out.error = "`" + type + "` is not a type there is a value of";
    return out;
  }
  out.stride = static_cast<int>(parts.size());

  // Which standard functions the window has, and where.
  const std::vector<Span> spans = Functions(lines);
  bool mesh[kMaxPieces] = {};
  bool vert[kMaxPieces] = {};
  std::vector<std::pair<Span, Stage>> standard;
  for (const Span& span : spans) {
    Standard found;
    std::string bad;
    if (!StandardName(span.name, &found, &bad)) {
      if (!bad.empty()) {
        out.error = bad;
        out.errorLine = span.first;
        return out;
      }
      continue;   // a helper, which every stage gets
    }
    standard.emplace_back(span, found.stage);
    if (found.base == "mesh") mesh[found.piece] = true;
    if (found.base == "vert") vert[found.piece] = true;
    if (found.base == "frag") out.frag[found.piece] = true;
    if (found.base == "position" && found.piece == 0) out.position = true;
    if (found.base == "grab") out.grab = true;
    if (found.base == "drag") out.drag = true;
  }
  if (!mesh[0]) {
    out.error = "a viz lays its geometry down in `void mesh()`";
    return out;
  }
  for (int k = 0; k < kMaxPieces; ++k) {
    if (mesh[k] != vert[k]) {
      const std::string n = (k == 0) ? "" : std::to_string(k + 1);
      out.error = mesh[k] ? "`mesh" + n + "` needs a `vert" + n + "` to place it"
                          : "`vert" + n + "` has no `mesh" + n + "` to place";
      for (const auto& [span, stage] : standard) {
        if (span.name == (mesh[k] ? "mesh" : "vert") + n) out.errorLine = span.first;
      }
      return out;
    }
    if (mesh[k]) out.pieces = k + 1;
    else break;
  }
  for (int k = out.pieces; k < kMaxPieces; ++k) {
    if (mesh[k] || out.frag[k]) {
      out.error = "the pieces are numbered without gaps: `mesh`, `mesh2`, `mesh3`";
      return out;
    }
  }

  // The window's own lines, operators rewritten. The sigil and the algebra line
  // are statements about the box and leave blanks behind.
  std::string joined;
  for (const std::string& line : lines) {
    const bool sigil = mathline::Parse(line).kind == mathline::Kind::Viz ||
                       clifford::ParseAlgebra(line, nullptr);
    joined += (sigil ? std::string() : line) + "\n";
  }
  std::string rewritten;
  if (!glslops::Rewrite(joined, &rewritten, nullptr) || rewritten.empty()) rewritten = joined;
  std::vector<std::string> body;
  {
    std::size_t from = 0;
    while (from < rewritten.size()) {
      const std::size_t end = rewritten.find('\n', from);
      if (end == std::string::npos) { body.push_back(rewritten.substr(from)); break; }
      body.push_back(rewritten.substr(from, end - from));
      from = end + 1;
    }
    body.resize(lines.size());
  }
  // Each stage gets the helpers and its own standard functions; the others'
  // lines are blanked, not removed.
  const auto bodyFor = [&](Stage stage) {
    std::vector<std::string> mine = body;
    for (const auto& [span, owner] : standard) {
      if (owner == stage) continue;
      for (int i = span.first; i <= span.last && i < static_cast<int>(mine.size()); ++i) {
        mine[static_cast<std::size_t>(i)].clear();
      }
    }
    std::string text;
    for (const std::string& line : mine) text += line + "\n";
    return text;
  };

  // --- what every stage is given -------------------------------------------
  std::string value = type + "(";
  std::string put;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    value += (i == 0 ? "" : ", ") + std::string("sbValues[b + ") + std::to_string(i) + "]";
    put += "  sbAnswers[at + " + std::to_string(i) + "] = v" +
           (parts[i].empty() ? std::string() : "." + parts[i]) + ";\n";
  }
  value += ")";

  std::string common =
      "layout(std430, binding = 2) readonly buffer SbValues { float sbValues[]; };\n"
      // Numbers per value, its colour first; how many values; where the pointer
      // is, in pixels from the window's corner; how big the window is; the
      // camera; and the pointer as a ray through the dw's own space.
      "uniform int sbStride;\n"
      "uniform int sbCount;\n"
      "uniform vec2 sbPointer;\n"
      "uniform vec2 sbSize;\n"
      "uniform mat4 sbViewProj;\n"
      "uniform vec3 sbEye;\n"
      "uniform vec3 sbRayOrigin;\n"
      "uniform vec3 sbRayDir;\n"
      // In a flat dw, how long one pixel of chrome is in the dw's own units:
      // chrome is drawn bigger as the canvas zooms in, and stays one size
      // however the dw itself is scaled.
      "uniform float sbPixel;\n"
      // Set by `vert` for something that is not *in* the space being cut, like a
      // point at infinity.
      "bool sbUncut = false;\n";
  common += glslops::BuiltinOverloads();
  // The algebra, two ways. The whole of it -- every type the window names, and
  // every product between them and what those products make -- is a hundred
  // kilobytes and a fifth of a second to compile, three programs over. A viz
  // that only reads a value's parts needs none of that but the structs, so it is
  // built with only those first, and with everything only if that fails.
  std::string head = common;
  std::string lean = common;
  common.clear();
  if (!algebraTypes.empty()) {
    head += clifford::GlslPrologue(sig, algebraTypes, false);
    lean += clifford::GlslStructs(sig, algebraTypes);
    out.lean = true;
  }
  if (mathline::IsComplexType(type) ||
      std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
        return l.find(types::complexnum::kName) != std::string::npos;
      })) {
    common += types::complexnum::GlslPrologue();
  }
  if (mathline::IsColorType(type) ||
      std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
        return l.find(types::color::kName) != std::string::npos;
      })) {
    common += types::color::GlslPrologue();
  }
  common += type + " sbValue(int i) {\n  int b = i * sbStride + " + std::to_string(kTintFloats) +
            ";\n  return " + value + ";\n}\n";
  common +=
      "vec4 sbTintOf(int i) {\n"
      "  int b = i * sbStride;\n"
      "  return vec4(sbValues[b], sbValues[b + 1], sbValues[b + 2], sbValues[b + 3]);\n"
      "}\n"
      // Where a point of the dw lands in the window, in pixels from its corner.
      "vec2 sbScreen(vec3 p) {\n"
      "  vec4 c = sbViewProj * vec4(p, 1.0);\n"
      "  vec2 n = c.xy / c.w;\n"
      "  return vec2((n.x * 0.5 + 0.5) * sbSize.x, (0.5 - n.y * 0.5) * sbSize.y);\n"
      "}\n"
      // Which way is across the screen, and which way up it, in the dw's own
      // space: what a marker that faces the viewer is laid out along, however the
      // dw has been turned.
      "vec3 sbAcross() {\n"
      "  return normalize(vec3(sbViewProj[0][0], sbViewProj[1][0], sbViewProj[2][0]));\n"
      "}\n"
      "vec3 sbUpward() {\n"
      "  return normalize(vec3(sbViewProj[0][1], sbViewProj[1][1], sbViewProj[2][1]));\n"
      "}\n";
  const std::string tail = common;

  const std::string cap = std::to_string(kMeshCap);
  const auto computeHead = [&](const std::string& common) {
    return
      "#version 430\n"
      "layout(local_size_x = 1) in;\n"
      "layout(std430, binding = 1) buffer SbMesh { uint sbMeshN; float sbMeshV[]; };\n"
      "layout(std430, binding = 3) buffer SbAnswers { float sbAnswers[]; };\n"
      "uniform int sbMode;\n"
      "uniform int sbPiece;\n"
      "uniform int sbOne;\n" +
      common +
      "void emit(vec3 p, vec3 n, vec2 uv) {\n"
      "  uint i = atomicAdd(sbMeshN, 1u);\n"
      "  if (i >= " + cap + "u) return;\n"
      "  uint b = i * 8u;\n"
      "  sbMeshV[b] = p.x; sbMeshV[b + 1u] = p.y; sbMeshV[b + 2u] = p.z;\n"
      "  sbMeshV[b + 3u] = n.x; sbMeshV[b + 4u] = n.y; sbMeshV[b + 5u] = n.z;\n"
      "  sbMeshV[b + 6u] = uv.x; sbMeshV[b + 7u] = uv.y;\n"
      "}\n"
      "void emit(vec3 p, vec3 n) { emit(p, n, vec2(0.0)); }\n"
      "void sbPut(int at, " + type + " v) {\n" + put + "}\n";
  };
  const std::string stub =
      "void emit(vec3 p, vec3 n, vec2 uv) {}\n"
      "void emit(vec3 p, vec3 n) {}\n";
  const auto vertexHead = [&](const std::string& common) {
    return
      "#version 430\n"
      "layout(std430, binding = 1) readonly buffer SbMesh { uint sbMeshN; float sbMeshV[]; };\n"
      "uniform int sbPiece;\n"
      "flat out int sbInstanceV;\n"
      "flat out vec4 sbTintV;\n"
      "flat out float sbUncutV;\n"
      "out vec3 sbPV;\n"
      "out vec3 sbNV;\n"
      "out vec2 sbUvV;\n" +
      common + stub;
  };
  // The light every viz gets unless it says otherwise: one directional light,
  // from over the viewer's shoulder so nothing goes black because the camera
  // came round to the other side of it. A flat dw has nothing to light, and
  // there a viz is simply its colour.
  const bool lit = (dw == "Space" || dw == "Spacetime");
  const auto fragmentHead = [&](const std::string& common) {
    return
      "#version 430\n"
      "uniform int sbPiece;\n"
      "uniform vec3 sbLimitsMin;\n"
      "uniform vec3 sbLimitsMax;\n"
      "uniform float sbCut;\n"
      "flat in int sbInstanceV;\n"
      "flat in vec4 sbTintV;\n"
      "flat in float sbUncutV;\n"
      "in vec3 sbPV;\n"
      "in vec3 sbNV;\n"
      "in vec2 sbUvV;\n"
      "out vec4 sbColour;\n"
      "vec4 sbTint;\n" +
      common + stub +
      "vec4 sbLit(vec3 p, vec3 n) {\n"
      "  vec3 toEye = normalize(sbEye - p);\n"
      "  vec3 toLight = normalize(vec3(0.4, 0.85, 0.35));\n"
      "  float lambert = max(dot(n, toLight), 0.0);\n"
      "  float back = max(dot(n, -toLight), 0.0) * 0.25;\n"
      "  float spec = pow(max(dot(toEye, reflect(-toLight, n)), 0.0), 24.0) * 0.35;\n"
      "  vec3 c = sbTint.rgb * (0.35 + 0.75 * lambert + back) + vec3(spec);\n"
      "  return vec4(pow(c, vec3(1.0 / 2.2)), sbTint.a);\n"
      "}\n";
  };
  const std::string compute = computeHead(head + tail);
  const std::string vertex = vertexHead(head + tail);
  const std::string fragment = fragmentHead(head + tail);

  // Every prologue padded to the longest, so the window's first line is the
  // same line of all three and one map reads a diagnostic from any of them.
  out.bodyAt = std::max({CountLines(compute), CountLines(vertex), CountLines(fragment)});
  const auto padded = [&](const std::string& head) {
    return head + std::string(static_cast<std::size_t>(out.bodyAt - CountLines(head)), '\n');
  };

  const auto numbered = [](const char* base, int k) {
    return std::string(base) + (k == 0 ? "" : std::to_string(k + 1));
  };

  // --- the compute program: the mesh, and the questions ---------------------
  std::string computeMain =
      "void main() {\n"
      "  int i = int(gl_WorkGroupID.x);\n"
      "  if (sbMode == 0) {\n";
  for (int k = 0; k < out.pieces; ++k) {
    computeMain += "    " + std::string(k == 0 ? "if" : "else if") + " (sbPiece == " +
                   std::to_string(k) + ") " + numbered("mesh", k) + "();\n";
  }
  computeMain +=
      "    return;\n"
      "  }\n"
      "  if (sbMode == 1) {\n"
      "    " + type + " v = sbValue(i);\n"
      "    vec3 at = vec3(0.0);\n"
      "    float g = 1.0e9;\n" +
      (out.position ? "    at = position(v);\n" : "") +
      (out.grab ? "    g = grab(v);\n" : "") +
      "    sbAnswers[i * 4] = at.x;\n"
      "    sbAnswers[i * 4 + 1] = at.y;\n"
      "    sbAnswers[i * 4 + 2] = at.z;\n"
      "    sbAnswers[i * 4 + 3] = g;\n"
      "    return;\n"
      "  }\n" +
      (out.drag ? "  if (sbMode == 2) sbPut(sbCount * 4, drag(sbValue(sbOne)));\n" : "") +
      "}\n";
  out.compute = padded(compute) + bodyFor(Stage::Compute) + computeMain;

  // --- the drawing ------------------------------------------------------------
  std::string vertexMain =
      "void main() {\n"
      "  int b = gl_VertexID * 8;\n"
      "  vec3 p = vec3(sbMeshV[b], sbMeshV[b + 1], sbMeshV[b + 2]);\n"
      "  vec3 n = vec3(sbMeshV[b + 3], sbMeshV[b + 4], sbMeshV[b + 5]);\n"
      "  vec2 uv = vec2(sbMeshV[b + 6], sbMeshV[b + 7]);\n"
      "  " + type + " v = sbValue(gl_InstanceID);\n";
  for (int k = 0; k < out.pieces; ++k) {
    vertexMain += "  " + std::string(k == 0 ? "if" : "else if") + " (sbPiece == " +
                  std::to_string(k) + ") " + numbered("vert", k) + "(v, p, n);\n";
  }
  vertexMain +=
      "  sbInstanceV = gl_InstanceID;\n"
      "  sbTintV = sbTintOf(gl_InstanceID);\n"
      "  sbUncutV = sbUncut ? 1.0 : 0.0;\n"
      "  sbPV = p;\n"
      "  sbNV = n;\n"
      "  sbUvV = uv;\n"
      "  gl_Position = sbViewProj * vec4(p, 1.0);\n"
      "}\n";
  out.vertex = padded(vertex) + bodyFor(Stage::Vertex) + vertexMain;

  std::string fragmentMain =
      "void main() {\n"
      "  sbTint = sbTintV;\n"
      // The dw's cut: what falls outside its walls is not there.
      "  bool outside = any(lessThan(sbPV, sbLimitsMin)) || any(greaterThan(sbPV, sbLimitsMax));\n"
      "  if (sbCut > 0.5 && sbUncutV < 0.5 && outside) discard;\n"
      "  vec3 n = normalize(sbNV);\n"
      "  vec4 c = " + std::string(lit ? "sbLit(sbPV, n)" : "sbTint") + ";\n"
      "  " + type + " v = sbValue(sbInstanceV);\n";
  for (int k = 0; k < out.pieces; ++k) {
    if (!out.frag[k]) continue;
    fragmentMain += "  if (sbPiece == " + std::to_string(k) + ") c = " + numbered("frag", k) +
                    "(v, sbPV, n, sbUvV);\n";
  }
  fragmentMain +=
      "  sbColour = c;\n"
      "}\n";
  out.fragment = padded(fragment) + bodyFor(Stage::Fragment) + fragmentMain;
  if (out.lean) {
    // Not padded: a lean build that fails is never reported -- the whole one is
    // built and reported instead -- so its line numbers are nobody's business.
    out.leanCompute = computeHead(lean + tail) + bodyFor(Stage::Compute) + computeMain;
    out.leanVertex = vertexHead(lean + tail) + bodyFor(Stage::Vertex) + vertexMain;
    out.leanFragment = fragmentHead(lean + tail) + bodyFor(Stage::Fragment) + fragmentMain;
  }
  return out;
}

}  // namespace sb::vizglsl
