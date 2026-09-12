#include "types/RealFunction.h"

#include <cstdio>
#include <string>

namespace sb::types::realfunction {
std::string ExampleBody(const std::string& name) {
  // Deterministic in the name, like every other example generator here.
  unsigned int hash = 2166136261u;
  for (char c : name) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  const float coefficient = 0.6f + 0.1f * static_cast<float>((hash >> 8) % 12u);
  char        number[32];
  std::snprintf(number, sizeof(number), "%.4g", static_cast<double>(coefficient));

  // Four recognisable shapes: a wave, a parabola, a bump, a line.
  switch (hash % 4u) {
    case 0: return std::string("sin(") + number + " * x)";
    case 1: return std::string(number) + " * x * x - 1.0";
    case 2: return std::string("exp(-") + number + " * x * x)";
    default: return std::string(number) + " * x";
  }
}

std::string ExampleGlsl(const std::string& name) {
  return "float " + name + "(float x) { return " + ExampleBody(name) + "; }";
}

}  // namespace sb::types::realfunction
