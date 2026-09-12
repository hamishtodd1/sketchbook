#include "types/Complex.h"

#include <cstdio>

#include "types/examples.h"

namespace sb::types::complexnum {

std::vector<std::string> MemberNames() { return {"re", "im"}; }

std::string GlslPrologue() {
  // Written as one raw string so it reads as the GLSL it is. Every one of these
  // is a name the operator rewrite produces, so the list is not a matter of
  // taste: leave one out and an ordinary-looking line stops compiling.
  return R"(
struct Complex { float re; float im; };
Complex add(Complex a, Complex b) { return Complex(a.re + b.re, a.im + b.im); }
Complex sub(Complex a, Complex b) { return Complex(a.re - b.re, a.im - b.im); }
Complex neg(Complex a) { return Complex(-a.re, -a.im); }
Complex gp(Complex a, Complex b) {
  return Complex(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re);
}
Complex gp(Complex a, float k) { return Complex(a.re * k, a.im * k); }
Complex gp(float k, Complex a) { return Complex(a.re * k, a.im * k); }
Complex add(Complex a, float k) { return Complex(a.re + k, a.im); }
Complex add(float k, Complex a) { return Complex(a.re + k, a.im); }
Complex sub(Complex a, float k) { return Complex(a.re - k, a.im); }
Complex sub(float k, Complex a) { return Complex(k - a.re, -a.im); }
Complex div(Complex a, float k) { return Complex(a.re / k, a.im / k); }
// The reverse of a one-generator algebra is the conjugate, so `~z` is what a
// mathematician would write and what this answers to.
Complex rev(Complex a) { return Complex(a.re, -a.im); }
float cabs(Complex a) { return sqrt(a.re * a.re + a.im * a.im); }
float carg(Complex a) { return atan(a.im, a.re); }
Complex div(Complex a, Complex b) {
  float d = b.re * b.re + b.im * b.im;
  return Complex((a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d);
}
Complex cexp(Complex a) {
  float e = exp(a.re);
  return Complex(e * cos(a.im), e * sin(a.im));
}
Complex clog(Complex a) { return Complex(log(cabs(a)), carg(a)); }
Complex cpow(Complex a, float k) {
  float r = pow(cabs(a), k);
  float t = carg(a) * k;
  return Complex(r * cos(t), r * sin(t));
}
)";
}

std::string ExampleValue(const std::string& name) {
  std::uint32_t hash = NameSeed(name);
  auto next = [&hash]() {
    hash ^= hash << 13;
    hash ^= hash >> 17;
    hash ^= hash << 5;
    return hash;
  };
  auto between = [&next](float low, float high) {
    const float t = static_cast<float>(next() % 1000u) / 1000.0f;
    return low + (high - low) * t;
  };
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "Complex(%.3f, %.3f)",
                static_cast<double>(between(-1.2f, 1.2f)),
                static_cast<double>(between(-1.2f, 1.2f)));
  return buffer;
}

}  // namespace sb::types::complexnum
