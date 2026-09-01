#include "compiler.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#include "process.h"

namespace sb::compiler {
namespace {

namespace fs = std::filesystem;

// Appended after the block's own text, so user line numbers are untouched.
// The exit code carries the answer: no stdio, nothing to parse.
constexpr const char* kEntryPoint = "\nint main() { return testFunc() ? 0 : 1; }\n";

constexpr int kCompileTimeoutMs = 25000;
constexpr int kRunTimeoutMs = 5000;

std::string ProbeCompiler() {
  std::vector<std::string> candidates;
  if (const char* override = std::getenv("SKETCHBOOK_CXX")) candidates.emplace_back(override);
  candidates.emplace_back("g++");
  candidates.emplace_back("clang++");
  // The app is usually launched from a shell or file manager that never had
  // MSYS2 on PATH, so check the usual install locations too.
  candidates.emplace_back("C:/msys64/mingw64/bin/g++.exe");
  candidates.emplace_back("C:/msys64/ucrt64/bin/g++.exe");
  candidates.emplace_back("C:/msys64/clang64/bin/clang++.exe");
  candidates.emplace_back("C:/mingw64/bin/g++.exe");

  for (const std::string& candidate : candidates) {
    const proc::Result probe = proc::Run(proc::Quote(candidate) + " --version", 8000);
    if (probe.started && probe.exitCode == 0) return candidate;
  }
  return {};
}

fs::path WorkDirectory() {
  std::error_code ec;
  fs::path dir = fs::temp_directory_path(ec) / "sketchbook-cxx";
  if (ec) dir = fs::path("sketchbook-cxx");
  fs::create_directories(dir, ec);
  return dir;
}

// Parses one diagnostic line of the form
//   <path>:<line>:<column>: error: <message>
// working backwards from the marker, because a Windows path has its own colons.
bool ParseDiagnosticLine(std::string_view line, Diagnostic* out) {
  std::size_t marker = line.find(": error: ");
  std::size_t skip = 9;
  if (marker == std::string_view::npos) {
    marker = line.find(": fatal error: ");
    skip = 15;
  }
  if (marker == std::string_view::npos) return false;

  out->present = true;
  out->message = std::string(line.substr(marker + skip));
  out->line = 1;
  out->column = 1;

  // The prefix is everything before the marker; peel trailing ":<n>" groups.
  std::string_view prefix = line.substr(0, marker);
  auto peelNumber = [&prefix](int* value) {
    const std::size_t colon = prefix.rfind(':');
    if (colon == std::string_view::npos) return false;
    std::string_view digits = prefix.substr(colon + 1);
    if (digits.empty()) return false;
    for (char c : digits) {
      if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    }
    *value = std::atoi(std::string(digits).c_str());
    prefix = prefix.substr(0, colon);
    return true;
  };

  int first = 0;
  if (peelNumber(&first)) {
    int second = 0;
    if (peelNumber(&second)) {
      out->line = second;   // <line>:<column>
      out->column = first;
    } else {
      out->line = first;    // only a line number was given
    }
  }
  return true;
}

// Returns the first error in the compiler's output. GCC and Clang both emit
// diagnostics in source order, so the first one is the earliest.
Diagnostic FirstError(const std::string& output) {
  Diagnostic diagnostic;
  std::size_t start = 0;
  while (start <= output.size()) {
    std::size_t end = output.find('\n', start);
    if (end == std::string::npos) end = output.size();
    std::string_view line(output.data() + start, end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (ParseDiagnosticLine(line, &diagnostic)) return diagnostic;
    if (end == output.size()) break;
    start = end + 1;
  }
  return diagnostic;
}

}  // namespace

const std::string& ToolchainPath() {
  static const std::string path = ProbeCompiler();
  return path;
}

Build Compile(const std::string& source, int lineOffset) {
  Build build;

  const std::string& compiler = ToolchainPath();
  if (compiler.empty()) {
    build.haveCompiler = false;
    build.detail = "No C++ compiler found. Install one, or point SKETCHBOOK_CXX at it.";
    return build;
  }

  // A fresh name each build: the previous executable may still be running.
  static std::atomic<unsigned> counter{0};
  const unsigned id = counter.fetch_add(1);
  const fs::path dir = WorkDirectory();
  const fs::path cppPath = dir / ("unit_" + std::to_string(id) + ".cpp");
  const fs::path exePath = dir / ("unit_" + std::to_string(id) + ".exe");

  {
    std::ofstream file(cppPath, std::ios::binary | std::ios::trunc);
    if (!file) {
      build.haveCompiler = false;
      build.detail = "Could not write to " + cppPath.string();
      return build;
    }
    file << source;
  }

  std::error_code ec;
  const std::string command = proc::Quote(compiler) +
                              " -std=c++20 -O0 -w -fdiagnostics-color=never -o " +
                              proc::Quote(exePath.string()) + " " +
                              proc::Quote(cppPath.string());
  const proc::Result result = proc::Run(command, kCompileTimeoutMs);
  fs::remove(cppPath, ec);

  if (!result.started) {
    build.haveCompiler = false;
    build.detail = "Could not launch " + compiler;
    return build;
  }
  if (result.exitCode != 0 || !fs::exists(exePath, ec)) {
    build.error = FirstError(result.output);
    if (!build.error.present) {
      // The compiler failed without a diagnostic we could locate; show whatever
      // it did say rather than nothing.
      build.error.present = true;
      build.error.message = result.timedOut ? "compiler timed out" : "compilation failed";
    }
    // Map the diagnostic back onto the user's own lines.
    build.error.line = std::max(1, build.error.line - lineOffset);
    fs::remove(exePath, ec);
    return build;
  }

  build.ok = true;
  build.exePath = exePath.string();
  return build;
}

Run Execute(const std::string& exePath, const std::vector<std::string>& arguments,
            int timeoutMs) {
  std::string command = proc::Quote(exePath);
  for (const std::string& argument : arguments) command += " " + proc::Quote(argument);

  const proc::Result result = proc::Run(command, timeoutMs);
  Run run;
  run.ok = result.started && !result.timedOut && result.exitCode == 0;
  run.timedOut = result.timedOut;
  run.exitCode = result.exitCode;
  run.output = result.output;
  return run;
}

void Discard(const std::string& exePath) {
  if (exePath.empty()) return;
  std::error_code ec;
  fs::remove(exePath, ec);
}

Outcome BuildAndRun(const std::string& source) {
  Outcome outcome;

  std::string unit = source;
  if (!unit.empty() && unit.back() != '\n') unit += '\n';
  unit += kEntryPoint;

  const Build build = Compile(unit, 0);
  if (!build.haveCompiler) {
    outcome.status = Status::NoCompiler;
    outcome.detail = build.detail;
    return outcome;
  }
  if (!build.ok) {
    outcome.status = Status::Failed;
    outcome.error = build.error;
    return outcome;
  }

  const Run run = Execute(build.exePath, {}, kRunTimeoutMs);
  if (run.exitCode == 0 && !run.timedOut) {
    outcome.status = Status::True;
  } else if (run.exitCode == 1 && !run.timedOut) {
    outcome.status = Status::False;
  } else if (run.timedOut) {
    outcome.status = Status::Crashed;
    outcome.detail = "testFunc() did not return within 5s";
  } else {
    outcome.status = Status::Crashed;
    outcome.detail = "The program exited with code " + std::to_string(run.exitCode);
  }

  Discard(build.exePath);
  return outcome;
}

}  // namespace sb::compiler
