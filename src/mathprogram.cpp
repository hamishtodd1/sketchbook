#include "mathprogram.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

#include "mathline.h"
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
std::string Prologue() {
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
      "int main(int argc, char** argv) {\n";
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
  const std::string prologue = Prologue();
  return static_cast<int>(std::count(prologue.begin(), prologue.end(), '\n'));
}

Program::~Program() {
  // Wait for anything in flight before the futures go out of scope.
  if (build_.valid()) build_.wait();
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

  std::vector<Variable> variables;
  std::string body;
  int freeIndex = 0;

  for (const std::string& raw : lines) {
    const mathline::Line line = mathline::Parse(raw);

    if (line.kind == mathline::Kind::Declaration && !line.hasInitialiser) {
      // The abstract declaration the user wrote; the initialiser is ours.
      Variable variable;
      variable.name = line.name;
      variable.latexName = mathline::NameToLatex(line.name);
      variable.kind = mathline::IsFunctionType(line.type) ? Kind::Function : Kind::Real;
      variable.free = true;

      if (variable.kind == Kind::Function) {
        // A function cannot travel down argv, so choosing one means writing it
        // into the source. That makes it a rebuild rather than a re-run, which
        // is why a function has no knob yet.
        variables.push_back(variable);
        body += "  " + line.type + " " + line.name + " = " +
                types::ExampleRealFunction(line.name) + ";\n";
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
      ++freeIndex;
      continue;
    }

    if (line.kind == mathline::Kind::Declaration && line.hasInitialiser) {
      Variable variable;
      variable.name = line.name;
      variable.latexName = mathline::NameToLatex(line.name);
      variable.kind = mathline::IsFunctionType(line.type) ? Kind::Function : Kind::Real;
      variable.free = false;
      variables.push_back(variable);
    }

    // Anything else is passed through verbatim: it is already valid C++, or it
    // is the user's mistake to see reported.
    body += "  " + raw + "\n";
  }

  if (variables.empty()) {
    variables_.clear();
    state_ = State::Empty;
    compiler::Discard(exePath_);
    exePath_.clear();
    return;
  }

  for (const Variable& variable : variables) {
    if (variable.kind == Kind::Function) {
      body += types::realfunction::PrintStatement(variable.name);
    } else {
      body += "  std::printf(\"" + variable.name + "=%.9g\\n\", (double)" + variable.name +
              ");\n";
    }
  }
  body += "  return 0;\n}\n";

  variables_ = std::move(variables);
  state_ = State::Building;
  error_ = {};
  detail_.clear();

  restore_.clear();

  const std::string source = Prologue() + body;
  const int offset = LineOffset();
  build_ = std::async(std::launch::async,
                      [source, offset] { return compiler::Compile(source, offset); });
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
      state_ = State::Failed;
      error_ = build.error;
    } else {
      compiler::Discard(exePath_);
      exePath_ = build.exePath;
      state_ = State::Ready;
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
