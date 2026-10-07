// Clifford algebras, generated as GLSL.
//
// This is the app's answer to GAmphetamine.js: given a signature, it writes out
// concrete structs and concrete products between them, fully unrolled, with no
// runtime dispatch. That is the same bargain the rest of this program makes --
// the app supplies the ends, the user writes the middle.
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
// geometric algebra; Cl(2,2) is Cl(2,2,0). The generators are counted from one,
// and `e0` is the null generator where there is one -- the way every PGA text
// numbers them -- so Cl(2,2) is e1 e2 e3 e4 and Cl(3,0,1) has e0 null and
// e1 e2 e3 positive.
//
// Operators follow GAmphetamine, because that is what the notation already is:
//
//     a * b    geometric product
//     a | b    inner product        (the symmetric one: grade |r - s|)
//     a ^ b    outer product        (grade r + s)
//     a & b    regressive product   (the dual of the outer product of duals)
//     a >> b   sandwich             (superconjugation: a b ~a, negated when
//                                    both sides are wholly odd, and typed as b
//                                    because a versor action preserves grade)
//     ~a       reverse
//
// and `dual(a)`, the Poincare dual -- a complement, so it survives a degenerate
// metric. It is a function and not `!a`, which is left to be boolean not.
//
// GLSL has no operator overloading, so what this file writes is only the named
// functions -- `gp`, `ip`, `op`, `rp`, `rev`, `sw`, `dual`, `add`, `sub`, `div`,
// `gradeK` -- and `glslops` rewrites the operators into them. The precedence is
// the mathematician's: every algebra operator binds tighter than `+` and `-`,
// so `a + b | c` is `a + (b | c)`, in the rewrite and in the reading alike.
//
// Like the rest of the mathematics this file knows nothing about raylib.
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

// Reads `Cl(3, 0, 1);` -- the one line that changes a box's algebra.
// Returns false if the line is not one of those.
bool ParseAlgebra(std::string_view line, Signature* out);

// True if `name` names a type of this algebra: a grade word, an ascending run
// of grade words, or one of Even / Odd / Multivector.
bool IsMultivectorType(std::string_view name, const Signature& sig);

// The types compiled unless a box mentions others.
const std::vector<std::string>& DefaultTypes();

// The GLSL for an algebra: a struct for every type in `wanted`, every type the
// products between them turn out to need, and every product between all of
// them. Nothing is written for a type nobody asked for.
// Only the structs of exactly these types, and their basis: no products, no
// sums, nothing a type does. A few hundred bytes where the whole algebra is a
// hundred kilobytes, for code that reads a value's parts and nothing more.
std::string GlslStructs(const Signature& sig, const std::vector<std::string>& wanted);

// `defaults` adds the types every ordinary box gets whether it names them or
// not; a viz window, which names what it draws, goes without.
std::string GlslPrologue(const Signature& sig, const std::vector<std::string>& wanted,
                         bool defaults = true);

// The names of every type mentioned in a piece of source, so that a box gets
// the products it uses and no others.
std::vector<std::string> TypesMentioned(std::string_view source, const Signature& sig);

// How `x is one of these` is written as mathematics.
std::string TypeLatex(std::string_view name, const Signature& sig);

// How the choice of algebra is written: the line that says which algebra a box
// works in is a statement of mathematics too, so it is set as one.
std::string AlgebraLatex(const Signature& sig);

// The members of a type, in the order its struct declares them -- `e012`,
// `e123`, and so on, with `s` for the scalar. Empty if the name is not a type
// of this algebra. What a value of it looks like from outside is exactly this
// list of floats, which is how one comes back in the trace and how a window
// reads it.
std::vector<std::string> MemberNames(std::string_view type, const Signature& sig);

// An example value of this type, as the GLSL that makes one. Deterministic in
// the variable's name, like every other example generator here.
std::string ExampleValue(std::string_view type, std::string_view name,
                         const Signature& sig);
// The same example as its numbers, one per member, which is what a knob holds.
std::vector<float> ExampleComponents(std::string_view type, std::string_view name,
                                     const Signature& sig);

// --- the algebra itself, exposed because the tests check it -----------------

// Multiplies two basis blades. Returns the sign (-1, 0 or +1) and writes the
// resulting blade to `out`. Zero only happens on a degenerate generator.
int BladeProduct(const Signature& sig, int a, int b, int* out);

// The number of generators in a blade.
int Grade(int blade);

}  // namespace sb::clifford
