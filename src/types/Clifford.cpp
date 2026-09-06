#include "types/Clifford.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

namespace sb::clifford {
namespace {

// The word for each grade -- the count and nothing else, since the "-vector"
// on the end of "bivector" says nothing that "Bi" does not. Short matters here:
// a compound type spells out every grade it holds, and `ScalarQuad` is a name
// where `ScalarQuadvector` is a sentence.
const char* const kGradeWords[] = {"Scalar", "Una",   "Bi",  "Tri",
                                   "Quad",   "Quint", "Sext"};
constexpr int kMaxGrade = static_cast<int>(sizeof(kGradeWords) / sizeof(kGradeWords[0])) - 1;

// A type is a set of grades, held as a bitmask over grade numbers.
using Grades = std::uint32_t;

// The most types one box will ever be given. A safety valve, not a design
// limit: the closure below is over subsets of grades and cannot get far.
constexpr std::size_t kMaxTypes = 48;

int PopCount(unsigned int value) {
  int count = 0;
  while (value != 0) {
    count += static_cast<int>(value & 1u);
    value >>= 1;
  }
  return count;
}

Grades EvenGrades(const Signature& sig) {
  Grades g = 0;
  for (int k = 0; k <= sig.n(); k += 2) g |= 1u << k;
  return g;
}

Grades OddGrades(const Signature& sig) {
  Grades g = 0;
  for (int k = 1; k <= sig.n(); k += 2) g |= 1u << k;
  return g;
}

Grades AllGrades(const Signature& sig) {
  Grades g = 0;
  for (int k = 0; k <= sig.n(); ++k) g |= 1u << k;
  return g;
}

// The name a grade-set is written with. Every grade-set has one, which is what
// keeps the generated types exact instead of widened.
std::string NameOfGrades(Grades grades, const Signature& sig) {
  if (grades == 0) return "float";
  if (grades == (1u << 0)) return "float";   // a scalar is a float and nothing else
  if (grades == AllGrades(sig)) return "Multivector";
  if (grades == EvenGrades(sig)) return "Even";
  if (grades == OddGrades(sig)) return "Odd";

  std::string name;
  for (int k = 0; k <= sig.n(); ++k) {
    if ((grades & (1u << k)) == 0) continue;
    name += (k <= kMaxGrade) ? kGradeWords[k] : ("Grade" + std::to_string(k));
  }
  return name;
}

// The reverse: what a written name means. Returns 0 if it means nothing here.
Grades GradesOfName(std::string_view name, const Signature& sig) {
  if (name == "Multivector") return AllGrades(sig);
  if (name == "Even") return EvenGrades(sig);
  if (name == "Odd") return OddGrades(sig);
  if (name == "Pseudoscalar") return 1u << sig.n();

  // Otherwise a run of grade words, ascending. Ascending because that is the
  // order a name is written back out in, and a type has to be spelled the way
  // it was generated to be the same type: `BiTri` is one, `TriBi` is not.
  Grades      grades = 0;
  std::size_t at = 0;
  int         last = -1;
  while (at < name.size()) {
    int matched = -1;
    for (int k = 0; k <= kMaxGrade && k <= sig.n(); ++k) {
      const std::string_view word(kGradeWords[k]);
      if (name.compare(at, word.size(), word) != 0) continue;
      matched = k;
      at += word.size();
      break;
    }
    // "Pseudoscalar" is allowed inside a compound too, meaning the top grade.
    if (matched < 0 && name.compare(at, 12, "Pseudoscalar") == 0) {
      matched = sig.n();
      at += 12;
    }
    if (matched < 0 || matched <= last) return 0;
    grades |= 1u << matched;
    last = matched;
  }
  return grades;
}

// Every blade of a grade-set, in the order they are written in a struct: by
// grade, then by index.
std::vector<int> BladesOf(Grades grades, const Signature& sig) {
  std::vector<int> blades;
  for (int k = 0; k <= sig.n(); ++k) {
    if ((grades & (1u << k)) == 0) continue;
    for (int blade = 0; blade < sig.blades(); ++blade) {
      if (Grade(blade) == k) blades.push_back(blade);
    }
  }
  return blades;
}

// What a blade is called as a member: the scalar is `s`, everything else is
// `e` and its generators, ascending.
std::string BladeName(int blade) {
  if (blade == 0) return "s";
  std::string name = "e";
  for (int i = 0; i < 32; ++i) {
    if ((blade >> i) & 1) name += static_cast<char>('0' + i);
  }
  return name;
}

// The sign of the reverse on a given grade.
int ReverseSign(int grade) { return ((grade * (grade - 1) / 2) % 2 == 0) ? 1 : -1; }

// The dual of a blade: its complement, signed so that b ^ dual(b) is the
// pseudoscalar. A complement rather than a multiplication by the pseudoscalar,
// so that it still works when the pseudoscalar squares to zero.
int DualOf(const Signature& sig, int blade, int* sign) {
  const int full = sig.blades() - 1;
  const int complement = full ^ blade;
  int       product = 0;
  *sign = BladeProduct(sig, blade, complement, &product);
  return complement;
}

// Which language the algebra is being written in. The difference that matters
// is that GLSL has no operator overloading, so `a * b` cannot be a geometric
// product there. Both targets therefore get the products as *functions* --
// GAmphetamine's own alternative spelling -- and C++ gets the operators as
// well. A box written in the function form runs in either language, which is
// what lets one box's text be tried as C++ and then as GLSL.
enum class Target { Cpp, Glsl };

// Which products are generated, and what each keeps.
enum class Product { Geometric, Inner, Outer, Regressive };

const char* OperatorOf(Product product) {
  switch (product) {
    case Product::Geometric: return "*";
    case Product::Inner: return "|";
    case Product::Outer: return "^";
    default: return "&";
  }
}

const char* FunctionOf(Product product) {
  switch (product) {
    case Product::Geometric: return "gp";
    case Product::Inner: return "ip";
    case Product::Outer: return "op";
    default: return "rp";
  }
}

// A term of a product: which component of the result, which components of the
// operands, and the sign.
struct Term {
  int result = 0;
  int left = 0;
  int right = 0;
  int sign = 0;
};

// Every non-zero term of one product between two blades.
void TermsFor(const Signature& sig, Product product, int a, int b, std::vector<Term>* out) {
  if (product == Product::Regressive) {
    // The dual of the outer product of the duals. Composed here rather than at
    // runtime, so it costs a multiply like everything else.
    int da = 0;
    int db = 0;
    const int leftDual = DualOf(sig, a, &da);
    const int rightDual = DualOf(sig, b, &db);
    if (da == 0 || db == 0) return;
    int       wedge = 0;
    const int sign = BladeProduct(sig, leftDual, rightDual, &wedge);
    if (sign == 0) return;
    if (Grade(wedge) != Grade(leftDual) + Grade(rightDual)) return;
    // Back again: undual is the map that takes dual(x) to x.
    const int full = sig.blades() - 1;
    const int back = full ^ wedge;
    int       undual = 0;
    int       ignored = 0;
    undual = BladeProduct(sig, back, wedge, &ignored);
    if (undual == 0) return;
    out->push_back({back, a, b, da * db * sign * undual});
    return;
  }

  int       result = 0;
  const int sign = BladeProduct(sig, a, b, &result);
  if (sign == 0) return;

  const int ga = Grade(a);
  const int gb = Grade(b);
  const int gr = Grade(result);
  if (product == Product::Inner && gr != std::abs(ga - gb)) return;
  if (product == Product::Outer && gr != ga + gb) return;
  out->push_back({result, a, b, sign});
}

// One generated type.
struct Type {
  Grades           grades = 0;
  std::string      name;
  std::vector<int> blades;
};

class Generator {
 public:
  Generator(const Signature& sig, Target target) : sig_(sig), target_(target) {}

  // Adds a type and everything the products between it and what is already
  // here turn out to need. Grade-sets are a small finite lattice, so this
  // always settles.
  void Want(Grades grades) {
    if (grades == 0 || grades == 1u) return;   // nothing, or a plain float
    if (types_.count(grades) != 0) return;
    if (types_.size() >= kMaxTypes) return;

    Type type;
    type.grades = grades;
    type.name = NameOfGrades(grades, sig_);
    type.blades = BladesOf(grades, sig_);
    types_.emplace(grades, std::move(type));

    // Everything this type can *make* with everything already here. Products
    // only: closing over addition as well would ask for the union of every
    // pair, which is the whole lattice of grade-sets and megabytes of code
    // nobody wanted. A sum of two unlike things is written when it is wanted --
    // name the type and the operator appears with it.
    std::vector<Grades> present;
    for (const auto& [key, value] : types_) present.push_back(key);
    for (Grades other : present) {
      for (const Product product : {Product::Geometric, Product::Inner, Product::Outer}) {
        Want(ResultOf(grades, other, product));
        Want(ResultOf(other, grades, product));
      }
    }
    // And what the dual of it is, since a dual with nowhere to land is an
    // operator you cannot use.
    Grades dual = 0;
    for (int blade : BladesOf(grades, sig_)) {
      int sign = 0;
      dual |= 1u << Grade(DualOf(sig_, blade, &sign));
    }
    Want(dual);
    // And each single grade it holds, so that grade selection has somewhere to
    // put its answer.
    for (int k = 0; k <= sig_.n(); ++k) {
      if ((grades & (1u << k)) != 0) Want(1u << k);
    }
  }

  std::string Emit() const;

 private:
  // The grades a product between two grade-sets can produce, worked out by
  // trying every pair of blades rather than by a rule -- a degenerate metric
  // takes terms away, and the rule would not know.
  Grades ResultOf(Grades left, Grades right, Product product) const {
    Grades       out = 0;
    const auto   a = BladesOf(left, sig_);
    const auto   b = BladesOf(right, sig_);
    std::vector<Term> terms;
    for (int x : a) {
      for (int y : b) {
        terms.clear();
        TermsFor(sig_, product, x, y, &terms);
        for (const Term& term : terms) out |= 1u << Grade(term.result);
      }
    }
    return out;
  }

  // The name to write for a grade-set: itself if it was generated, otherwise
  // the smallest generated type that contains it. Products are closed exactly
  // so they always find themselves; sums are not, and widening beats a missing
  // operator. Empty if there is nothing that will hold it.
  std::string BestName(Grades grades) const {
    if (grades == 0 || grades == 1u) return "float";
    if (types_.count(grades) != 0) return types_.at(grades).name;
    const Type* best = nullptr;
    for (const auto& [candidate, type] : types_) {
      if ((grades & ~candidate) != 0) continue;
      if (best == nullptr || PopCount(candidate) < PopCount(best->grades)) best = &type;
    }
    return best == nullptr ? std::string() : best->name;
  }

  void EmitStruct(std::ostringstream& out, const Type& type) const;
  void EmitProduct(std::ostringstream& out, const Type& left, const Type& right,
                   Product product) const;
  void EmitSum(std::ostringstream& out, const Type& left, const Type& right, bool add) const;
  void EmitUnary(std::ostringstream& out, const Type& type) const;
  void EmitGrades(std::ostringstream& out, const Type& type) const;
  void EmitScalar(std::ostringstream& out, const Type& type) const;

  // Writes one function. The name is the same in both languages; C++ takes its
  // arguments by reference, GLSL by value, which is the only other difference.
  void Define(std::ostringstream& out, const std::string& ret, const std::string& name,
              const std::string& left, const std::string& right, const std::string& body) const {
    out << Inline() << ret << " " << name << "(";
    if (cpp()) {
      out << "const " << left << "& a";
      if (!right.empty()) out << ", const " << right << "& b";
    } else {
      out << left << " a";
      if (!right.empty()) out << ", " << right << " b";
    }
    out << ") {\n" << body << "}\n\n";
  }

  // A list of zeros, one per component: what an all-components constructor
  // needs to make a value that is nothing.
  std::string Fill(Grades grades) const {
    std::string out;
    for (std::size_t i = 0; i < BladesOf(grades, sig_).size(); ++i) {
      out += (i == 0 ? "" : ", ") + Zero();
    }
    return out;
  }

  bool        cpp() const { return target_ == Target::Cpp; }
  std::string Zero() const { return cpp() ? "0.0f" : "0.0"; }
  std::string One() const { return cpp() ? "1.0f" : "1.0"; }
  const char* Inline() const { return cpp() ? "inline " : ""; }

  Signature                 sig_;
  Target                    target_ = Target::Cpp;
  std::map<Grades, Type>    types_;
};

void Generator::EmitStruct(std::ostringstream& out, const Type& type) const {
  out << "struct " << type.name << " {\n  float";
  for (std::size_t i = 0; i < type.blades.size(); ++i) {
    // GLSL has no default member initialisers, so nothing is written there and
    // every generated function fills in all of its components instead.
    out << (i == 0 ? " " : ", ") << BladeName(type.blades[i]) << (cpp() ? " = 0.0f" : "");
  }
  out << ";\n";
  if (!cpp()) {
    out << "};\n\n";
    return;
  }
  out << "  " << type.name << "() = default;\n";
  // Every component at once, in the order they are declared -- which is the
  // constructor GLSL gives a struct for free, so one spelling makes a value in
  // either language.
  out << "  " << type.name << "(";
  for (std::size_t i = 0; i < type.blades.size(); ++i) {
    out << (i == 0 ? "" : ", ") << "float a" << i;
  }
  out << ")";
  for (std::size_t i = 0; i < type.blades.size(); ++i) {
    out << (i == 0 ? " : " : ", ") << BladeName(type.blades[i]) << "(a" << i << ")";
  }
  out << " {}\n";

  // A float goes in as the scalar part, if there is one.
  if ((type.grades & 1u) != 0) {
    out << "  " << type.name << "(float value) : s(value) {}\n";
  }
  // And anything narrower goes in as itself. Not explicit: writing
  // `Even m = b;` for a bivector b is exactly what one means.
  for (const auto& [grades, other] : types_) {
    if (grades == type.grades) continue;
    if ((grades & ~type.grades) != 0) continue;
    out << "  " << type.name << "(const " << other.name << "& v)";
    for (std::size_t i = 0; i < other.blades.size(); ++i) {
      out << (i == 0 ? " : " : ", ") << BladeName(other.blades[i]) << "(v."
          << BladeName(other.blades[i]) << ")";
    }
    out << " {}\n";
  }
  out << "};\n\n";
}

void Generator::EmitProduct(std::ostringstream& out, const Type& left, const Type& right,
                            Product product) const {
  const Grades resultGrades = ResultOf(left.grades, right.grades, product);
  const std::string resultName = NameOfGrades(resultGrades, sig_);

  // Gather the terms once, so an empty product can be written as a plain zero.
  std::map<int, std::vector<Term>> byComponent;
  std::vector<Term>                terms;
  for (int a : left.blades) {
    for (int b : right.blades) {
      terms.clear();
      TermsFor(sig_, product, a, b, &terms);
      for (const Term& term : terms) byComponent[term.result].push_back(term);
    }
  }

  if (resultName != "float" && types_.count(resultGrades) == 0) return;

  // The body, written once. Which name it goes under is the only thing the two
  // languages disagree about.
  std::ostringstream body;
  if (byComponent.empty()) {
    // Every term of it vanishes -- a degenerate metric can do that. C++ warns
    // about the operands going unused; GLSL has no cast to void to say so with.
    if (cpp()) body << "  (void)a; (void)b;\n";
    body << "  return "
         << (resultName == "float" ? Zero() : (resultName + "(" + Fill(resultGrades) + ")"))
         << ";\n";
  } else if (resultName == "float") {
    body << "  return";
    const auto& only = byComponent.begin()->second;
    for (std::size_t i = 0; i < only.size(); ++i) {
      body << (only[i].sign < 0 ? " - " : (i == 0 ? " " : " + ")) << "a." << BladeName(only[i].left)
           << " * b." << BladeName(only[i].right);
    }
    body << ";\n";
  } else {
    body << "  " << resultName << " r;\n";
    for (int blade : BladesOf(resultGrades, sig_)) {
      const auto found = byComponent.find(blade);
      if (found == byComponent.end()) {
        // GLSL has no default member initialisers, so every component of the
        // answer is written whether it has terms or not.
        if (!cpp()) body << "  r." << BladeName(blade) << " = " << Zero() << ";\n";
        continue;
      }
      body << "  r." << BladeName(blade) << " =";
      const auto& list = found->second;
      for (std::size_t i = 0; i < list.size(); ++i) {
        body << (list[i].sign < 0 ? " - " : (i == 0 ? " " : " + ")) << "a."
             << BladeName(list[i].left) << " * b." << BladeName(list[i].right);
      }
      body << ";\n";
    }
    body << "  return r;\n";
  }

  Define(out, resultName, std::string(FunctionOf(product)), left.name, right.name, body.str());
  if (cpp()) {
    out << "inline " << resultName << " operator" << OperatorOf(product) << "(const "
        << left.name << "& a, const " << right.name << "& b) { return " << FunctionOf(product)
        << "(a, b); }\n\n";
  }
}

void Generator::EmitSum(std::ostringstream& out, const Type& left, const Type& right,
                        bool add) const {
  const Grades      resultGrades = left.grades | right.grades;
  const std::string resultName = BestName(resultGrades);
  if (resultName.empty() || resultName == "float") return;
  std::ostringstream body;
  body << "  " << resultName << " r;\n";
  for (int blade : BladesOf(GradesOfName(resultName, sig_), sig_)) {
    const bool inLeft = (left.grades & (1u << Grade(blade))) != 0;
    const bool inRight = (right.grades & (1u << Grade(blade))) != 0;
    body << "  r." << BladeName(blade) << " = ";
    if (inLeft && inRight) {
      body << "a." << BladeName(blade) << (add ? " + b." : " - b.") << BladeName(blade);
    } else if (inLeft) {
      body << "a." << BladeName(blade);
    } else if (inRight) {
      body << (add ? "b." : "-b.") << BladeName(blade);
    } else {
      body << Zero();   // only reached when the sum had to widen
    }
    body << ";\n";
  }
  body << "  return r;\n";

  Define(out, resultName, add ? "add" : "sub", left.name, right.name, body.str());
  if (cpp()) {
    out << "inline " << resultName << " operator" << (add ? "+" : "-") << "(const " << left.name
        << "& a, const " << right.name << "& b) { return " << (add ? "add" : "sub")
        << "(a, b); }\n\n";
  }
}

void Generator::EmitUnary(std::ostringstream& out, const Type& type) const {
  // Negation.
  {
    std::ostringstream body;
    body << "  " << type.name << " r;\n";
    for (int blade : type.blades) {
      body << "  r." << BladeName(blade) << " = -a." << BladeName(blade) << ";\n";
    }
    body << "  return r;\n";
    Define(out, type.name, "neg", type.name, "", body.str());
    if (cpp()) {
      out << "inline " << type.name << " operator-(const " << type.name
          << "& a) { return neg(a); }\n\n";
    }
  }

  // Reverse.
  {
    std::ostringstream body;
    body << "  " << type.name << " r;\n";
    for (int blade : type.blades) {
      body << "  r." << BladeName(blade) << " = " << (ReverseSign(Grade(blade)) < 0 ? "-" : "")
           << "a." << BladeName(blade) << ";\n";
    }
    body << "  return r;\n";
    Define(out, type.name, "rev", type.name, "", body.str());
    if (cpp()) {
      out << "inline " << type.name << " operator~(const " << type.name
          << "& a) { return rev(a); }\n\n";
    }
  }

  // Dual. The complement, so it survives a pseudoscalar that squares to zero.
  Grades dualGrades = 0;
  for (int blade : type.blades) {
    int sign = 0;
    dualGrades |= 1u << Grade(DualOf(sig_, blade, &sign));
  }
  const std::string dualName = NameOfGrades(dualGrades, sig_);
  if (types_.count(dualGrades) != 0 || dualName == "float") {
    std::ostringstream body;
    if (dualName == "float") {
      int sign = 0;
      const int from = type.blades.empty() ? 0 : type.blades.front();
      DualOf(sig_, from, &sign);
      body << "  return " << (sign < 0 ? "-" : "") << "a." << BladeName(from) << ";\n";
    } else {
      body << "  " << dualName << " r;\n";
      for (int blade : type.blades) {
        int       sign = 0;
        const int target = DualOf(sig_, blade, &sign);
        body << "  r." << BladeName(target) << " = " << (sign < 0 ? "-" : "") << "a."
             << BladeName(blade) << ";\n";
      }
      body << "  return r;\n";
    }
    Define(out, dualName, "dual", type.name, "", body.str());
    if (cpp()) {
      out << "inline " << dualName << " operator!(const " << type.name
          << "& a) { return dual(a); }\n\n";
    }
  }
}

// Grade selection: the part of a value that has a given grade. Written out per
// grade rather than as one function taking a number, because the *type* of the
// answer depends on which grade is asked for -- `grade2` of an even element is
// a bivector, and there is no one type that could stand for all of them.
//
// Only the grades a type actually has get a selector. Asking an `Even` for its
// grade 3 is a mistake worth being told about, and it is a mistake the type
// system can see; that it would be zero is beside the point.
void Generator::EmitGrades(std::ostringstream& out, const Type& type) const {
  for (int k = 0; k <= sig_.n(); ++k) {
    if ((type.grades & (1u << k)) == 0) continue;
    const Grades      one = 1u << k;
    const std::string resultName = NameOfGrades(one, sig_);
    if (resultName != "float" && types_.count(one) == 0) continue;

    std::ostringstream body;
    if (resultName == "float") {
      body << "  return a.s;\n";
    } else {
      body << "  " << resultName << " r;\n";
      for (int blade : BladesOf(one, sig_)) {
        body << "  r." << BladeName(blade) << " = a." << BladeName(blade) << ";\n";
      }
      body << "  return r;\n";
    }
    Define(out, resultName, "grade" + std::to_string(k), type.name, "", body.str());
  }
}

void Generator::EmitScalar(std::ostringstream& out, const Type& type) const {
  // A number on one side. In C++ these are operators; in both languages they
  // are the same named functions, so a box written in the function form runs
  // either way.
  const std::string self = cpp() ? ("const " + type.name + "& a") : (type.name + " a");
  const auto define = [&](const std::string& ret, const std::string& name, bool numberFirst,
                          const std::string& body) {
    out << Inline() << ret << " " << name << "("
        << (numberFirst ? ("float k, " + self) : (self + ", float k")) << ") {\n"
        << body << "}\n\n";
  };
  const auto op = [&](const char* symbol, const char* name, bool numberFirst) {
    if (!cpp()) return;
    out << "inline " << type.name << " operator" << symbol << "("
        << (numberFirst ? ("float k, " + self) : (self + ", float k")) << ") { return " << name
        << "(" << (numberFirst ? "k, a" : "a, k") << "); }\n\n";
  };

  // Scaling. The geometric, inner and outer products with a number are all the
  // same thing: a number has grade zero, so |r - 0| and r + 0 are both r.
  std::ostringstream scale;
  scale << "  " << type.name << " r;\n";
  for (int blade : type.blades) {
    scale << "  r." << BladeName(blade) << " = a." << BladeName(blade) << " * k;\n";
  }
  scale << "  return r;\n";
  for (const char* name : {"gp", "ip", "op"}) {
    define(type.name, name, false, scale.str());
    define(type.name, name, true, scale.str());
  }
  op("*", "gp", false);
  op("*", "gp", true);
  op("|", "ip", false);
  op("|", "ip", true);
  op("^", "op", false);
  op("^", "op", true);

  std::ostringstream divide;
  divide << "  " << type.name << " r;\n";
  for (int blade : type.blades) {
    divide << "  r." << BladeName(blade) << " = a." << BladeName(blade) << " / k;\n";
  }
  divide << "  return r;\n";
  define(type.name, "div", false, divide.str());
  op("/", "div", false);

  // Adding a number reaches the scalar part, so the result has one. Written as
  // an exact match rather than left to conversions: two types that both accept
  // a bare number would make the call ambiguous.
  const std::string sumName = BestName(type.grades | 1u);
  if (sumName.empty() || sumName == "float") return;
  const Grades sumGrades = GradesOfName(sumName, sig_);
  for (int side = 0; side < 2; ++side) {
    for (int minus = 0; minus < 2; ++minus) {
      std::ostringstream body;
      body << "  " << sumName << " r;\n";
      for (int blade : BladesOf(sumGrades, sig_)) {
        const bool mine = (type.grades & (1u << Grade(blade))) != 0;
        body << "  r." << BladeName(blade) << " = ";
        if (blade == 0 && mine) {
          body << (side == 0 ? "k " : "a.s ") << (minus ? "- " : "+ ")
               << (side == 0 ? "a.s" : "k");
        } else if (blade == 0) {
          body << (side == 0 ? "k" : (minus ? "-k" : "k"));
        } else if (mine) {
          body << ((minus && side == 0) ? "-a." : "a.") << BladeName(blade);
        } else {
          body << Zero();
        }
        body << ";\n";
      }
      body << "  return r;\n";
      define(sumName, minus ? "sub" : "add", side == 0, body.str());
      if (cpp()) {
        out << "inline " << sumName << " operator" << (minus ? "-" : "+") << "("
            << (side == 0 ? ("float k, " + self) : (self + ", float k")) << ") { return "
            << (minus ? "sub" : "add") << "(" << (side == 0 ? "k, a" : "a, k") << "); }\n\n";
      }
    }
  }
}

std::string Generator::Emit() const {
  std::ostringstream out;
  out << "// Cl(" << sig_.p << "," << sig_.q << "," << sig_.r
      << "), written out by the app. Every type here is a set of grades, and\n"
      << "// every product between them is exact.\n\n";

  // C++ wants them named before the functions that mention them; GLSL has no
  // forward declaration of a struct, and does not need one.
  if (cpp()) {
    for (const auto& [grades, type] : types_) out << "struct " << type.name << ";\n";
    out << "\n";
  }
  for (const auto& [grades, type] : types_) EmitStruct(out, type);

  for (const auto& [grades, type] : types_) {
    EmitUnary(out, type);
    EmitScalar(out, type);
    EmitGrades(out, type);
  }

  for (const auto& [leftGrades, left] : types_) {
    for (const auto& [rightGrades, right] : types_) {
      for (const Product product : {Product::Geometric, Product::Inner, Product::Outer,
                                    Product::Regressive}) {
        EmitProduct(out, left, right, product);
      }
      EmitSum(out, left, right, true);
      EmitSum(out, left, right, false);
    }
  }

  // The basis, so that a value can be written rather than assembled.
  out << "// The basis, so a value can be written out rather than assembled.\n";
  for (int blade = 1; blade < sig_.blades(); ++blade) {
    const Grades      grades = 1u << Grade(blade);
    const std::string name = NameOfGrades(grades, sig_);
    if (types_.count(grades) == 0) continue;
    if (cpp()) {
      out << "inline const " << name << " " << BladeName(blade) << " = [] { " << name
          << " v; v." << BladeName(blade) << " = 1.0f; return v; }();\n";
      continue;
    }
    // GLSL wants a constant expression, which a struct's own constructor is.
    out << "const " << name << " " << BladeName(blade) << " = " << name << "(";
    const std::vector<int> blades = BladesOf(grades, sig_);
    for (std::size_t i = 0; i < blades.size(); ++i) {
      out << (i == 0 ? "" : ", ") << (blades[i] == blade ? One() : Zero());
    }
    out << ");\n";
  }
  out << "\n";
  return out.str();
}

}  // namespace

// ---------------------------------------------------------------- the algebra

int Grade(int blade) { return PopCount(static_cast<unsigned int>(blade)); }

int BladeProduct(const Signature& sig, int a, int b, int* out) {
  *out = a ^ b;

  // The sign from sorting the two runs of generators together: one swap for
  // every pair that starts out in the wrong order.
  int swaps = 0;
  for (int i = 0; i < sig.n(); ++i) {
    if (((a >> i) & 1) == 0) continue;
    swaps += PopCount(static_cast<unsigned int>(b) & ((1u << i) - 1u));
  }
  int sign = (swaps % 2 == 0) ? 1 : -1;

  // And the metric, for the generators they share.
  const int common = a & b;
  for (int i = 0; i < sig.n(); ++i) {
    if (((common >> i) & 1) == 0) continue;
    sign *= sig.square(i);
  }
  return sign;
}

// ------------------------------------------------------------------ the names

bool IsMultivectorType(std::string_view name, const Signature& sig) {
  return GradesOfName(name, sig) != 0;
}

const std::vector<std::string>& DefaultTypes() {
  // What a box gets without asking. Everything else is compiled when a box
  // turns out to mention it.
  static const std::vector<std::string> types = {"Una", "Bi", "Even"};
  return types;
}

bool ParseAlgebra(std::string_view line, Signature* out) {
  std::size_t at = line.find_first_not_of(" \t");
  if (at == std::string_view::npos) return false;
  if (line.compare(at, 8, "Algebra(") != 0) return false;
  at += 8;

  int values[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    while (at < line.size() && (line[at] == ' ' || line[at] == ',')) ++at;
    if (at >= line.size() || std::isdigit(static_cast<unsigned char>(line[at])) == 0) return false;
    int value = 0;
    while (at < line.size() && std::isdigit(static_cast<unsigned char>(line[at])) != 0) {
      value = value * 10 + (line[at] - '0');
      ++at;
    }
    values[i] = value;
  }
  if (values[0] + values[1] + values[2] < 1 || values[0] + values[1] + values[2] > 6) return false;
  if (out != nullptr) *out = Signature{values[0], values[1], values[2]};
  return true;
}

std::vector<std::string> TypesMentioned(std::string_view source, const Signature& sig) {
  std::vector<std::string> found;
  std::size_t              at = 0;
  while (at < source.size()) {
    if (std::isalpha(static_cast<unsigned char>(source[at])) == 0 &&
        source[at] != '_') {
      ++at;
      continue;
    }
    const std::size_t begin = at;
    while (at < source.size() && (std::isalnum(static_cast<unsigned char>(source[at])) != 0 ||
                                  source[at] == '_')) {
      ++at;
    }
    const std::string word(source.substr(begin, at - begin));
    if (!IsMultivectorType(word, sig)) continue;
    if (std::find(found.begin(), found.end(), word) == found.end()) found.push_back(word);
  }
  return found;
}

std::string GlslPrologue(const Signature& sig, const std::vector<std::string>& wanted) {
  Generator generator(sig, Target::Glsl);
  for (const std::string& name : DefaultTypes()) generator.Want(GradesOfName(name, sig));
  for (const std::string& name : wanted) generator.Want(GradesOfName(name, sig));
  return generator.Emit();
}

std::string Prologue(const Signature& sig, const std::vector<std::string>& wanted) {
  Generator generator(sig, Target::Cpp);
  for (const std::string& name : DefaultTypes()) generator.Want(GradesOfName(name, sig));
  for (const std::string& name : wanted) generator.Want(GradesOfName(name, sig));
  return generator.Emit();
}

std::string AlgebraLatex(const Signature& sig) {
  char signature[64];
  std::snprintf(signature, sizeof(signature), "%d,%d,%d", sig.p, sig.q, sig.r);
  // The algebra gets a name, and everything written afterwards is written in
  // terms of it. A script A, so an ordinary variable called A is a different
  // letter and there is nothing to collide with -- and it is named as the
  // Clifford algebra it is, not as the space underneath it.
  return std::string("\\mathscr{A} = \\mathcal{C}\\ell^{") + signature + "}";
}

std::string TypeLatex(std::string_view name, const Signature& sig) {
  const Grades grades = GradesOfName(name, sig);

  // Once the algebra has a name, every type is said in terms of it: the whole
  // of it, its two halves under the even/odd grading, or an exterior power.
  //
  // The exterior power is of the *space* the algebra is built on rather than of
  // the algebra itself, which is a mild abuse -- but a standard one, and the
  // usual justification holds: a Clifford algebra is isomorphic to the exterior
  // algebra of that space as a graded vector space, differing only in its
  // product, so the grade-k part of one is the grade-k part of the other.
  if (grades == AllGrades(sig)) return "\\mathscr{A}";
  if (grades == EvenGrades(sig)) return "\\mathscr{A}^{+}";
  if (grades == OddGrades(sig)) return "\\mathscr{A}^{-}";

  // A single grade is that exterior power; a compound is their sum.
  std::string latex;
  for (int k = 0; k <= sig.n(); ++k) {
    if ((grades & (1u << k)) == 0) continue;
    if (!latex.empty()) latex += " \\oplus ";
    // Grade zero is the reals; every other grade is that exterior power of the
    // algebra. A compound is the direct sum of its parts, which is what it is.
    latex += (k == 0) ? std::string("\\mathbb{R}")
                      : ("\\bigwedge^{" + std::to_string(k) + "}\\mathscr{A}");
  }
  return latex.empty() ? std::string("\\mathbb{R}") : latex;
}

std::vector<std::string> MemberNames(std::string_view type, const Signature& sig) {
  const Grades grades = GradesOfName(type, sig);
  if (grades == 0) return {};
  std::vector<std::string> out;
  for (int blade : BladesOf(grades, sig)) out.push_back(BladeName(blade));
  return out;
}

std::string ExampleValue(std::string_view type, std::string_view name, const Signature& sig) {
  // Deterministic in the name, like every other example generator here: the
  // same variable gets the same value every time the box is built.
  std::uint32_t seed = 2166136261u;
  for (char c : name) {
    seed ^= static_cast<unsigned char>(c);
    seed *= 16777619u;
  }

  const Grades     grades = GradesOfName(type, sig);
  std::vector<int> blades = BladesOf(grades, sig);
  if (blades.empty()) return "0.0";

  // Written as the constructor every component at once, because that is the
  // one spelling both languages accept: C++ is given such a constructor and
  // GLSL gives a struct one for free.
  std::string out(type);
  out += "(";
  for (std::size_t i = 0; i < blades.size(); ++i) {
    seed = seed * 1664525u + 1013904223u;
    const float value =
        std::round((static_cast<float>((seed >> 8) % 2001u) / 1000.0f - 1.0f) * 100.0f) / 100.0f;
    char term[32];
    std::snprintf(term, sizeof(term), "%s%.2f", i == 0 ? "" : ", ", static_cast<double>(value));
    out += term;
  }
  out += ")";
  return out;
}

}  // namespace sb::clifford
