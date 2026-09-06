#include "types/RealFunction.h"

#include <cstddef>
#include <string>
#include <utility>

namespace sb::types::realfunction {
namespace {

// Written out as a raw string so it reads as the C++ it is rather than as a
// wall of escaped quotes. Every line of it lands in the user's program.
const char* const kSource = R"CPP(
struct RealFunction : std::function<float(float)> {
  using std::function<float(float)>::function;
};
// There is deliberately no arithmetic on functions here any more. Making them
// a vector space needed `RealFunction` to be a `std::function` with operators
// over it, and a function is GLSL now: `h = f + 2 * g` is written
// `RealFunction h(float x) { return f(x) + 2.0 * g(x); }`, which composes by
// calling rather than by overloading and costs nothing at run time.
static void sb_plot(const char* name, const RealFunction& f) {
  std::printf("%s=", name);
  for (int i = 0; i < SB_SAMPLES; ++i) {
    float t = SB_LO + (SB_HI - SB_LO) * (float)i / (float)(SB_SAMPLES - 1);
    std::printf(" %.6g", (double)f(t));
  }
  std::printf("\n");
}
)CPP";

// The sample window is the app's to choose, so it is substituted in rather than
// written twice.
std::string Substituted() {
  std::string out;
  const std::string source = kSource;
  const std::pair<std::string, std::string> swaps[] = {
      {"SB_SAMPLES", std::to_string(kSampleCount)},
      {"SB_LO", std::to_string(kSampleLo) + "f"},
      {"SB_HI", std::to_string(kSampleHi) + "f"},
  };

  for (std::size_t i = 0; i < source.size();) {
    bool replaced = false;
    for (const auto& [token, value] : swaps) {
      if (source.compare(i, token.size(), token) != 0) continue;
      out += value;
      i += token.size();
      replaced = true;
      break;
    }
    if (!replaced) out += source[i++];
  }
  return out;
}

}  // namespace

const char* Source() {
  // Built once: the substitution is the same every time, and the caller wants a
  // stable pointer.
  static const std::string source = Substituted();
  return source.c_str();
}

std::string ExampleGlsl(const std::string& name) {
  // Deterministic in the name, like every other example generator here.
  unsigned int hash = 2166136261u;
  for (char c : name) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  const float coefficient = 0.6f + 0.1f * static_cast<float>((hash >> 8) % 12u);
  char        number[32];
  std::snprintf(number, sizeof(number), "%.4g", static_cast<double>(coefficient));

  std::string expression;
  switch (hash % 4u) {
    case 0: expression = std::string("sin(") + number + " * x)"; break;
    case 1: expression = std::string(number) + " * x * x - 1.0"; break;
    case 2: expression = std::string("exp(-") + number + " * x * x)"; break;
    default: expression = std::string(number) + " * x"; break;
  }
  return "float " + name + "(float x) { return " + expression + "; }";
}

std::string PrintStatement(const std::string& name) {
  return "  sb_plot(\"" + name + "\", " + name + ");\n";
}

}  // namespace sb::types::realfunction
