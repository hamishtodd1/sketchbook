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
inline RealFunction operator+(RealFunction a, RealFunction b) {
  return [a, b](float x) { return a(x) + b(x); };
}
inline RealFunction operator-(RealFunction a, RealFunction b) {
  return [a, b](float x) { return a(x) - b(x); };
}
inline RealFunction operator-(RealFunction a) {
  return [a](float x) { return -a(x); };
}
inline RealFunction operator*(float k, RealFunction a) {
  return [k, a](float x) { return k * a(x); };
}
inline RealFunction operator*(RealFunction a, float k) {
  return [a, k](float x) { return a(x) * k; };
}
inline RealFunction operator*(RealFunction a, RealFunction b) {
  return [a, b](float x) { return a(x) * b(x); };
}
inline RealFunction operator/(RealFunction a, float k) {
  return [a, k](float x) { return a(x) / k; };
}
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

std::string PrintStatement(const std::string& name) {
  return "  sb_plot(\"" + name + "\", " + name + ");\n";
}

}  // namespace sb::types::realfunction
