// Example generators: how each type produces a concrete value to be going on
// with.
//
// This is the pivotal move of the whole program made mechanical. A declaration
// like `float a;` or `RealFunction f;` is abstract, and a program cannot be
// about reals in general -- so the app chooses one. Each type says how.
//
// The generators are *defined* by each type's viz file, since choosing a value
// and showing it are two halves of the same job. They are declared here, apart
// from those files, because the toolchain has to ask for an example and the
// toolchain does not include raylib.
//
// Deterministic in the variable's name, so a variable keeps the same example
// between runs instead of jittering every time something is rebuilt.
#pragma once

#include <cstdint>
#include <string_view>

namespace sb::types {

// A real: a tidy number.
float ExampleReal(std::string_view name);

// The seed every generator starts from. FNV-1a over the name.
inline std::uint32_t NameSeed(std::string_view name) {
  std::uint32_t hash = 2166136261u;
  for (char c : name) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  return hash;
}

}  // namespace sb::types
