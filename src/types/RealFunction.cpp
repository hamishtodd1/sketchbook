#include "types/RealFunction.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "types/Complex.h"
#include "types/examples.h"

namespace sb::types::realfunction {
namespace {

// One number to shape the answer with, made out of whatever the function takes.
// A point is asked how far out it is, which gives every function of space
// something radial to be; a complex number likewise. A multivector has no
// obvious one, so its example does not vary -- that is a gap, not a choice.
std::string Driver(std::string_view type, std::string_view param) {
  const std::string p(param);
  if (type == "float" || type == "double") return p;
  if (type == "vec2" || type == "vec3" || type == "vec4") return "length(" + p + ")";
  if (type == complexnum::kName) return "cabs(" + p + ")";
  return "1.0";
}

// How many numbers make up an answer of this type, and so how many shapes the
// constructor wants.
std::size_t Components(std::string_view type, const clifford::Signature& sig) {
  if (type == "vec2" || type == "vec3" || type == "vec4") {
    return static_cast<std::size_t>(type.back() - '0');
  }
  if (type == complexnum::kName) return complexnum::MemberNames().size();
  if (clifford::IsMultivectorType(type, sig)) return clifford::MemberNames(type, sig).size();
  return 1;
}

// One of four recognisable shapes of `d`: a wave, a parabola, a bump, a line.
std::string Shape(std::uint32_t seed, const std::string& d) {
  const float coefficient = 0.6f + 0.1f * static_cast<float>((seed >> 8) % 12u);
  char        number[32];
  std::snprintf(number, sizeof(number), "%.1f", static_cast<double>(coefficient));
  switch (seed % 4u) {
    case 0: return std::string("sin(") + number + " * " + d + ")";
    case 1: return std::string(number) + " * " + d + " * " + d + " - 1.0";
    case 2: return std::string("exp(-") + number + " * " + d + " * " + d + ")";
    default: return std::string(number) + " * " + d;
  }
}

}  // namespace

std::string ExampleFunctionBody(std::string_view returnType, std::string_view paramType,
                                std::string_view paramName, std::string_view name,
                                const clifford::Signature& sig) {
  const std::string d = Driver(paramType, paramName);
  std::uint32_t     seed = NameSeed(name);
  const std::size_t count = Components(returnType, sig);
  if (count <= 1) return Shape(seed, d);

  // Every component at once, which is the one constructor every type here has
  // -- GLSL gives a struct one for free. Each component takes the next number
  // off the seed, so they are different shapes rather than one shape repeated.
  std::string out(returnType);
  out += "(";
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) out += ", ";
    out += Shape(seed, d);
    seed = seed * 1664525u + 1013904223u;
  }
  return out + ")";
}

}  // namespace sb::types::realfunction
