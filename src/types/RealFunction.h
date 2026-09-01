// RealFunction: a function from reals to reals.
//
// This is the type itself -- the C++ the app prepends to every program so that
// `RealFunction f;` compiles -- and nothing about how one is drawn. That lives
// next door in RealFunctionViz.*.
//
// The definition is deliberately thin. `RealFunction` *is* a
// std::function<float(float)>, so there is nothing to learn and nothing to take
// on trust: it is exactly what a C++ programmer already means by a function
// from floats to floats. On top of that sit the operators that make these
// functions a vector space.
//
// No raylib here, and none in any type: the mathematics and the toolchain do
// not know that a renderer exists.
#pragma once

#include <string>

namespace sb::types::realfunction {

// How the type is spelled in the user's C++.
inline constexpr const char* kName = "RealFunction";

// The C++ prepended to every program that uses one: the type, its operators,
// and the sampler that prints one back to the app.
const char* Source();

// The statement that prints a variable of this type back. A real prints one
// number; a function has to print its whole sample run, because there is no
// other way to send a function to something that is going to draw it.
std::string PrintStatement(const std::string& name);

// The window a function is sampled over, and how finely. Fixed at compile time
// and wider than a plot shows by default, so zooming a plot out still has curve
// to draw.
inline constexpr int   kSampleCount = 193;
inline constexpr float kSampleLo = -8.0f;
inline constexpr float kSampleHi = 8.0f;

// The input value the i-th sample was taken at.
inline float SampleInput(int i) {
  return kSampleLo + (kSampleHi - kSampleLo) * static_cast<float>(i) /
                         static_cast<float>(kSampleCount - 1);
}

}  // namespace sb::types::realfunction
