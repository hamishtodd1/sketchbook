#include "mathprogram.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

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
std::string ShaderPrologue(const std::vector<Variable>& variables, int* lines) {
  std::string out = std::string("#version 330\n") + "in vec2 " + kShaderInput +
                    ";\n"
                    "in vec4 fragColor;\n"
                    "out vec4 finalColor;\n"
                    "uniform sampler2D texture0;\n"
                    "uniform vec4 colDiffuse;\n";
  // A free real reaches a program through argv and a shader through a uniform.
  // Either way the knob on its number line is what moves it.
  for (const Variable& variable : variables) {
    if (variable.free && variable.kind == Kind::Real) {
      out += "uniform float " + variable.name + ";\n";
    }
  }
  out += "void main() {\n";
  *lines = static_cast<int>(std::count(out.begin(), out.end(), '\n'));
  return out;
}

// A shader *can* hand a value back: render the value instead of the picture,
// into a target one pixel per variable, and read the pixels. What it cannot do
// is hand back a value that does not depend on where you look -- inside a
// fragment shader a variable is a field, not a number -- so the point to read
// at is a uniform, and choosing it is the caller's problem.
//
// This is the same body over again with different ends: the varying replaced by
// that point, and the user's own write to finalColor overwritten by ours.
std::string ProbePrologue(const std::vector<Variable>& variables) {
  std::string out =
      "#version 330\n"
      "in vec4 fragColor;\n"
      "out vec4 finalColor;\n"
      "uniform sampler2D texture0;\n"
      "uniform vec4 colDiffuse;\n"
      "uniform vec2 sbProbe;\n";
  for (const Variable& variable : variables) {
    if (variable.free && variable.kind == Kind::Real) {
      out += "uniform float " + variable.name + ";\n";
    }
  }
  out += "void main() {\n";
  // Not the interpolated varying: the one point being asked about.
  out += std::string("  vec2 ") + kShaderInput + " = sbProbe;\n";
  return out;
}

std::string ProbeEpilogue(const std::vector<std::string>& probed) {
  std::string out = "  float sbValue = 0.0;\n  int sbIndex = int(gl_FragCoord.x);\n";
  for (std::size_t i = 0; i < probed.size(); ++i) {
    out += "  if (sbIndex == " + std::to_string(i) + ") sbValue = " + probed[i] + ";\n";
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

void Program::Rebuild(const std::vector<std::string>& lines) {
  if (build_.valid()) return;  // one build at a time

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
  for (const std::string& raw : lines) {
    clifford::ParseAlgebra(raw, &signature);
    joined += raw;
    joined += '\n';
  }
  const std::vector<std::string> algebraTypes = clifford::TypesMentioned(joined, signature);

  std::vector<Variable> variables;
  std::string body;
  // The same lines assembled for the other language, kept in step line for line
  // with the C++ so a diagnostic from either lands on the same row of the box.
  std::string shaderBody;
  int freeIndex = 0;

  for (const std::string& raw : lines) {
    // The line that chooses the algebra is a statement about the box, not a
    // statement in it. It leaves a blank behind rather than vanishing, so a
    // diagnostic still lands on the row the user wrote.
    if (clifford::ParseAlgebra(raw, nullptr)) {
      body += "\n";
      shaderBody += "\n";
      continue;
    }

    const mathline::Line line = mathline::Parse(raw, signature);

    if (line.kind == mathline::Kind::Declaration && !line.hasInitialiser) {
      // The abstract declaration the user wrote; the initialiser is ours.
      Variable variable;
      variable.name = line.name;
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
        body += "  " + line.type + " " + line.name + " = " +
                clifford::ExampleValue(line.type, line.name, signature) + ";\n";
        shaderBody += "\n";
        continue;
      }

      if (variable.kind == Kind::Function) {
        // A function cannot travel down argv, so choosing one means writing it
        // into the source. That makes it a rebuild rather than a re-run, which
        // is why a function has no knob yet.
        variables.push_back(variable);
        body += "  " + line.type + " " + line.name + " = " +
                types::ExampleRealFunction(line.name) + ";\n";
        shaderBody += "\n";   // functions are not a shader's business
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
      variables.push_back(variable);

      body += "  " + line.type + " " + line.name + " = sb_input(argc, argv, " +
              std::to_string(freeIndex) + ", 0.0f);\n";
      // Its uniform is declared above main, so the line itself goes blank --
      // blank rather than absent, to keep the line numbering honest.
      shaderBody += "\n";
      ++freeIndex;
      continue;
    }

    if (line.kind == mathline::Kind::Declaration && line.hasInitialiser) {
      Variable variable;
      variable.name = line.name;
      variable.latexName = mathline::NameToLatex(line.name);
      variable.kind = mathline::IsFunctionType(line.type)
                          ? Kind::Function
                          : (mathline::IsMultivectorType(line.type, signature) ? Kind::Multivector
                                                                              : Kind::Real);
      variable.free = false;
      variables.push_back(variable);
    }

    // Anything else is passed through verbatim: it is already valid C++, or it
    // is the user's mistake to see reported.
    body += "  " + raw + "\n";
    shaderBody += "  " + raw + "\n";
  }

  if (variables.empty()) {
    variables_.clear();
    state_ = State::Empty;
    compiler::Discard(exePath_);
    exePath_.clear();
    // A box with nothing in it should be holding nothing.
    glsl_.clear();
    glslProbe_.clear();
    probed_.clear();
    return;
  }

  for (const Variable& variable : variables) {
    if (variable.kind == Kind::Multivector) {
      // Nothing is reported back for one yet: there is no window that shows a
      // multivector, so there is nothing waiting to be told.
      continue;
    }
    if (variable.kind == Kind::Function) {
      body += types::realfunction::PrintStatement(variable.name);
    } else {
      body += "  std::printf(\"" + variable.name + "=%.9g\\n\", (double)" + variable.name +
              ");\n";
    }
  }
  body += "  return 0;\n}\n";

  // Assembled now, alongside the C++, and used only if the C++ is refused.
  int prologueLines = 0;
  glsl_ = ShaderPrologue(variables, &prologueLines) + shaderBody + "}\n";
  glslOffset_ = prologueLines;
  looksLikeGlsl_ = MentionsShaderWord(shaderBody);

  // A free real is already known -- its knob is what set it. The derived ones
  // are what have to be asked for.
  probed_.clear();
  for (const Variable& variable : variables) {
    if (!variable.free && variable.kind == Kind::Real) probed_.push_back(variable.name);
  }
  glslProbe_ = probed_.empty()
                   ? std::string()
                   : (ProbePrologue(variables) + shaderBody + ProbeEpilogue(probed_));

  variables_ = std::move(variables);
  state_ = State::Building;
  error_ = {};
  detail_.clear();

  restore_.clear();

  const std::string source = CxxPrologue(signature, algebraTypes) + body;
  const int offset = LineOffset();
  build_ = std::async(std::launch::async,
                      [source, offset] { return compiler::Compile(source, offset); });
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
  const compiler::Diagnostic shaderError = compiler::ParseGlslLog(log, glslOffset_);
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

void Program::SetValue(std::size_t index, float value) {
  if (index >= variables_.size()) return;
  if (!variables_[index].free || variables_[index].kind != Kind::Real) return;
  variables_[index].value = value;
  Launch();
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
      glslProbe_.clear();
      probed_.clear();
      Launch();  // first run, to fill in the derived values
    }
  }

  if (run_.valid() && run_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const RunOutput output = run_.get();
    run_ = {};

    if (output.ok) {
      for (const RunValue& printed : output.values) {
        for (Variable& variable : variables_) {
          if (variable.name != printed.name) continue;
          if (variable.kind == Kind::Function) {
            // Nothing on screen owns a function's samples, free or not: the
            // program is the only thing that knows what the curve looks like.
            variable.samples = printed.numbers;
            variable.known = !variable.samples.empty();
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

}  // namespace sb::mathprogram
