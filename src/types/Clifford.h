// Clifford algebras, generated as plain C++.
//
// This is the C++ answer to GAmphetamine.js: given a signature, it writes out
// concrete structs and concrete products between them, fully unrolled, with no
// templates and no runtime dispatch. That is the same bargain the rest of this
// program makes -- the app supplies the ends, the user writes the middle -- and
// it is the reason a box using geometric algebra still compiles in the time it
// takes to let go of a key.
//
// **A type is a set of grades.** Not an arbitrary set of basis blades: grades.
// That one restriction is what makes the whole thing tidy, because the product
// of two grade-sets is a grade-set, so every product has an exact result type
// and every result type has a name. `Bi` is grade 2; `Even` is every even
// grade; `ScalarQuad` is what it says. Nothing is ever widened to
// `Multivector` to make the types line up.
//
// The names are the count and nothing else: the "-vector" on the end of
// "bivector" says nothing that "Bi" does not, and a compound type spells out
// every grade it holds, so `ScalarQuad` is a name where `ScalarQuadvector` is a
// sentence. They run ascending, because that is the order a name is written
// back out in and a type has to be spelled the way it was generated.
//
// The **signature** is written the way it is spoken: Cl(p, q, r) has p
// generators squaring to +1, q to -1 and r to 0, and Cl(3,0,1) is 3D projective
// geometric algebra. The generators are indexed the way every PGA text indexes
// them -- the null ones first -- so Cl(3,0,1) has e0 null and e1 e2 e3 positive.
//
// Operators follow GAmphetamine, because that is what the notation already is:
//
//     a * b    geometric product
//     a | b    inner product        (the symmetric one: grade |r - s|)
//     a ^ b    outer product        (grade r + s)
//     a & b    regressive product   (the dual of the outer product of duals)
//     ~a       reverse
//     !a       dual                 (Poincare: a complement, so it survives a
//                                    degenerate metric)
//
// C++ gives `^` and `|` lower precedence than `+`, so `a + b | c` parses as
// `(a + b) | c`. That is a fact about C++ and not something to fix here; write
// the brackets.
//
// Like the rest of the toolchain layer this file knows nothing about raylib.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sb::clifford {

// p generators squaring to +1, q to -1, r to 0.
struct Signature {
  int p = 3;
  int q = 0;
  int r = 1;

  int n() const { return p + q + r; }
  int blades() const { return 1 << n(); }
  // What generator `i` squares to. The null ones come first.
  int square(int i) const { return (i < r) ? 0 : (i < r + p ? 1 : -1); }
};

// The algebra a box works in unless it says otherwise: 3D projective geometric
// algebra, which is what these tools are mostly for.
inline constexpr Signature kDefaultSignature{3, 0, 1};

// Reads `Algebra(3, 0, 1);` -- the one line that changes a box's algebra.
// Returns false if the line is not one of those.
bool ParseAlgebra(std::string_view line, Signature* out);

// True if `name` names a type of this algebra: a grade word, an ascending run
// of grade words, or one of Even / Odd / Multivector.
bool IsMultivectorType(std::string_view name, const Signature& sig);

// The types compiled unless a box mentions others.
const std::vector<std::string>& DefaultTypes();

// The C++ for an algebra: a struct for every type in `wanted`, every type the
// products between them turn out to need, and every product between all of
// them. Nothing is written for a type nobody asked for.
std::string Prologue(const Signature& sig, const std::vector<std::string>& wanted);

// The names of every type mentioned in a piece of source, so that a box gets
// the products it uses and no others.
std::vector<std::string> TypesMentioned(std::string_view source, const Signature& sig);

// How `x is one of these` is written as mathematics.
std::string TypeLatex(std::string_view name, const Signature& sig);

// How the choice of algebra is written: the line that says which algebra a box
// works in is a statement of mathematics too, so it is set as one.
std::string AlgebraLatex(const Signature& sig);

// An example value of this type, as the C++ that makes one. Deterministic in
// the variable's name, like every other example generator here.
std::string ExampleValue(std::string_view type, std::string_view name,
                         const Signature& sig);

// --- the algebra itself, exposed because the tests check it -----------------

// Multiplies two basis blades. Returns the sign (-1, 0 or +1) and writes the
// resulting blade to `out`. Zero only happens on a degenerate generator.
int BladeProduct(const Signature& sig, int a, int b, int* out);

// The number of generators in a blade.
int Grade(int blade);

}  // namespace sb::clifford
