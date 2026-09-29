#include "mathline.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "types/Color.h"
#include "types/Complex.h"

namespace sb::mathline {
namespace {

// ------------------------------------------------------------------- tokens
enum class Tok {
  End, Number, Ident, Plus, Minus, Star, Slash, LParen, RParen,
  LBracket, RBracket, LBrace, RBrace, Comma, Assign, Semi, Amp, Dot,
  // The geometric algebra operators. They are C's bitwise ones; a Clifford
  // algebra is what gives them these meanings.
  Bar, Caret, Tilde,
  // `>>`, the sandwich: what a versor does to something.
  Shift
};

struct Token {
  Tok         kind = Tok::End;
  std::string text;
};

bool IsIdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}
bool IsIdentPart(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::vector<Token> Tokenize(std::string_view source, bool* ok) {
  std::vector<Token> tokens;
  *ok = true;
  std::size_t i = 0;
  while (i < source.size()) {
    const char c = source[i];
    if (std::isspace(static_cast<unsigned char>(c)) != 0) { ++i; continue; }

    if (IsIdentStart(c)) {
      const std::size_t start = i;
      while (i < source.size() && IsIdentPart(source[i])) ++i;
      tokens.push_back({Tok::Ident, std::string(source.substr(start, i - start))});
      continue;
    }
    // A `.` is a decimal point only when a digit follows it. Otherwise it is
    // reaching into a value for one of its parts, which is what lets the x and
    // the y of a point be written about at all.
    const bool pointsAtDigit =
        (i + 1 < source.size()) && std::isdigit(static_cast<unsigned char>(source[i + 1])) != 0;
    if (std::isdigit(static_cast<unsigned char>(c)) != 0 || (c == '.' && pointsAtDigit)) {
      const std::size_t start = i;
      while (i < source.size() &&
             (std::isdigit(static_cast<unsigned char>(source[i])) != 0 || source[i] == '.')) {
        ++i;
      }
      // A trailing float suffix is how code spells a float, not mathematics.
      if (i < source.size() && (source[i] == 'f' || source[i] == 'F')) ++i;
      std::string text(source.substr(start, i - start));
      if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) text.pop_back();
      // So is the rest of how a float is spelled: `2.` and `2.0` are both the
      // number two, and that is written `2`.
      if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
        if (text.empty()) text = "0";   // `.0` and `0.0` alike
      }
      tokens.push_back({Tok::Number, text});
      continue;
    }

    // `>>` is two characters, and the only one here that is.
    if (c == '>' && i + 1 < source.size() && source[i + 1] == '>') {
      tokens.push_back({Tok::Shift, ">>"});
      i += 2;
      continue;
    }

    Tok kind = Tok::End;
    switch (c) {
      case '+': kind = Tok::Plus; break;
      case '-': kind = Tok::Minus; break;
      case '*': kind = Tok::Star; break;
      case '|': kind = Tok::Bar; break;
      case '^': kind = Tok::Caret; break;
      case '~': kind = Tok::Tilde; break;
      case '/': kind = Tok::Slash; break;
      case '(': kind = Tok::LParen; break;
      case ')': kind = Tok::RParen; break;
      case '[': kind = Tok::LBracket; break;
      case ']': kind = Tok::RBracket; break;
      case '{': kind = Tok::LBrace; break;
      case '}': kind = Tok::RBrace; break;
      case ',': kind = Tok::Comma; break;
      case '.': kind = Tok::Dot; break;
      case '&': kind = Tok::Amp; break;
      case '=': kind = Tok::Assign; break;
      case ';': kind = Tok::Semi; break;
      default:
        *ok = false;   // anything else is outside the subset
        return tokens;
    }
    tokens.push_back({kind, std::string(1, c)});
    ++i;
  }
  tokens.push_back({Tok::End, {}});
  return tokens;
}

// --------------------------------------------------------------------- AST
enum class NodeKind {
  Number, Ident, Call, Member, Add, Sub, Mul, Div, Neg,
  // Inner, outer and regressive products, and the two unary ones. Written the
  // way a geometric algebra text writes them: a dot, a wedge, a vee, a tilde
  // over the thing and a star after it.
  Inner, Outer, Regressive, Reverse,
  // The sandwich, `a >> b`, which is written out as the sandwich it is.
  Sandwich
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
  NodeKind    kind = NodeKind::Number;
  std::string text;   // Number / Ident / Call: the name
  NodePtr     lhs, rhs;
  std::vector<NodePtr> args;   // Call
};

// Binding powers, used both to parse and to decide where LaTeX needs brackets.
int Precedence(NodeKind kind) {
  switch (kind) {
    // Tighter than anything, including the unary operators: `-p.x` is the
    // negation of a component, never a component of a negation.
    case NodeKind::Member: return 7;
    // **The mathematician's precedence, not C's.** Every algebra operator binds
    // tighter than + and -, so `a + b | c` is `a + (b | c)` -- which is what it
    // reads as. This used to follow C, which puts the products *below* the
    // sum, so the reading and the shader disagreed about what a line meant:
    // `glslops`, which is what actually runs, had the mathematician's ordering
    // first.
    case NodeKind::Add:
    case NodeKind::Sub: return 1;
    case NodeKind::Sandwich: return 2;
    case NodeKind::Inner: return 3;
    case NodeKind::Outer:
    case NodeKind::Regressive: return 4;
    case NodeKind::Mul:
    case NodeKind::Div: return 5;
    case NodeKind::Neg:
    case NodeKind::Reverse: return 6;
    default: return 7;  // atoms
  }
}

class Parser {
 public:
  explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

  NodePtr ParseExpression() { return ParseSum(); }
  bool    ok() const { return ok_; }
  const Token& Peek() const { return tokens_[pos_]; }
  void    Advance() { if (tokens_[pos_].kind != Tok::End) ++pos_; }
  bool    Accept(Tok kind) {
    if (tokens_[pos_].kind != kind) return false;
    Advance();
    return true;
  }

 private:
  // The loosest first: every algebra operator binds tighter than + and -, and
  // the sandwich is looser than the products it is made of, so `R >> a * b` is
  // the versor acting on the product. The same ordering `glslops` uses, since
  // one of them decides what a line reads as and the other what it does.
  NodePtr ParseSum() {
    NodePtr node = ParseSandwich();
    while (ok_ && (Peek().kind == Tok::Plus || Peek().kind == Tok::Minus)) {
      const NodeKind kind = (Peek().kind == Tok::Plus) ? NodeKind::Add : NodeKind::Sub;
      Advance();
      NodePtr rhs = ParseSandwich();
      node = Combine(kind, std::move(node), std::move(rhs));
    }
    return node;
  }

  NodePtr ParseSandwich() {
    NodePtr node = ParseInner();
    while (ok_ && Peek().kind == Tok::Shift) {
      Advance();
      node = Combine(NodeKind::Sandwich, std::move(node), ParseInner());
    }
    return node;
  }

  NodePtr ParseInner() {
    NodePtr node = ParseOuter();
    while (ok_ && Peek().kind == Tok::Bar) {
      Advance();
      node = Combine(NodeKind::Inner, std::move(node), ParseOuter());
    }
    return node;
  }

  // The wedge and the vee are one tier, taken left to right, which is what
  // equal binding powers mean where the shader's rewrite is concerned.
  NodePtr ParseOuter() {
    NodePtr node = ParseProduct();
    while (ok_ && (Peek().kind == Tok::Caret || Peek().kind == Tok::Amp)) {
      const NodeKind kind =
          (Peek().kind == Tok::Caret) ? NodeKind::Outer : NodeKind::Regressive;
      Advance();
      node = Combine(kind, std::move(node), ParseProduct());
    }
    return node;
  }

  NodePtr ParseProduct() {
    NodePtr node = ParseUnary();
    while (ok_ && (Peek().kind == Tok::Star || Peek().kind == Tok::Slash)) {
      const NodeKind kind = (Peek().kind == Tok::Star) ? NodeKind::Mul : NodeKind::Div;
      Advance();
      NodePtr rhs = ParseUnary();
      node = Combine(kind, std::move(node), std::move(rhs));
    }
    return node;
  }

  NodePtr ParseUnary() {
    if (Peek().kind == Tok::Tilde) {
      Advance();
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::Reverse;
      node->lhs = ParseUnary();
      return node;
    }
    if (Peek().kind == Tok::Minus) {
      Advance();
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::Neg;
      node->lhs = ParseUnary();
      return node;
    }
    if (Peek().kind == Tok::Plus) {  // a redundant unary plus
      Advance();
      return ParseUnary();
    }
    return ParsePrimary();
  }

  NodePtr ParsePrimary() {
    auto node = std::make_unique<Node>();
    if (Peek().kind == Tok::Number) {
      node->kind = NodeKind::Number;
      node->text = Peek().text;
      Advance();
      return node;
    }
    if (Peek().kind == Tok::Ident) {
      node->text = Peek().text;
      Advance();
      if (Peek().kind != Tok::LParen) {
        node->kind = NodeKind::Ident;
        return Parts(std::move(node));
      }
      // `sin(x)`, `pow(x, 2)`, or a call of the user's own function.
      Advance();
      node->kind = NodeKind::Call;
      if (Peek().kind != Tok::RParen) {
        node->args.push_back(ParseSum());
        while (ok_ && Accept(Tok::Comma)) node->args.push_back(ParseSum());
      }
      if (!Accept(Tok::RParen)) ok_ = false;
      return Parts(std::move(node));
    }
    if (Accept(Tok::LParen)) {
      NodePtr inner = ParseSum();
      if (!Accept(Tok::RParen)) ok_ = false;
      return Parts(std::move(inner));
    }
    ok_ = false;
    return node;
  }

  // Whatever is reached into after it: `p.x`, and `q.e012.something` if anyone
  // ever writes such a thing.
  NodePtr Parts(NodePtr node) {
    while (Peek().kind == Tok::Dot) {
      Advance();
      if (Peek().kind != Tok::Ident) {
        ok_ = false;
        return node;
      }
      auto part = std::make_unique<Node>();
      part->kind = NodeKind::Member;
      part->text = Peek().text;
      part->lhs = std::move(node);
      Advance();
      node = std::move(part);
    }
    return node;
  }

  NodePtr Combine(NodeKind kind, NodePtr lhs, NodePtr rhs) {
    auto node = std::make_unique<Node>();
    node->kind = kind;
    node->lhs = std::move(lhs);
    node->rhs = std::move(rhs);
    return node;
  }

  const std::vector<Token>& tokens_;
  std::size_t               pos_ = 0;
  bool                      ok_ = true;
};

// ------------------------------------------------------------------- LaTeX
//
// `marks`, when given, collects the identifiers written out and each one is
// wrapped in \sbref{k}{...} so the renderer can find it again. Pass nullptr to
// get the same LaTeX without the tags -- which is how two factors are compared
// for equality, since a tag makes otherwise identical renderings differ.
std::string Emit(const Node& node, int parentPrecedence, std::vector<std::string>* marks);

// Which equals sign a line has earned.
//
// In code `=` always *makes* the left side equal to the right, which is what a
// mathematician writes `:=` for. But not every such line is a rule: one whose
// right-hand side is a bare number fixes a value once and for all, and "let
// a = 0.4" is how that is written. So:
//
//   the right-hand side mentions something  ->  `:=`, a rule that recomputes
//   the right-hand side is a constant       ->  `=`,  a value being fixed
//
// TeX has no single glyph for it, so it is composed, the way LaTeX itself is
// obliged to.
// Two relations set side by side, which TeX spaces at nothing at all -- the gap
// between them is the glyphs' own bearings, and that is what makes the pair
// read as one symbol rather than as a colon that has fallen onto an equals.
const char* Defines(bool rule) { return rule ? " := " : " = "; }

// Whether an expression calls anything that is not a constructor. A call is a
// mention even with nothing inside the brackets: `randChanging()` is a new number
// every frame and `time`-like functions are exactly the lines that are not fixed,
// so they are rules. A constructor -- `vec3(1.0, 0.0, 0.0)`, `Tri(...)` -- only
// spells a value, and is as fixed as the numbers in it.
bool CallsAnything(const Node& node) {
  if (node.kind == NodeKind::Call && !node.text.empty() && !IsVectorType(node.text) &&
      std::isupper(static_cast<unsigned char>(node.text[0])) == 0) {
    return true;
  }
  if (node.lhs && CallsAnything(*node.lhs)) return true;
  if (node.rhs && CallsAnything(*node.rhs)) return true;
  for (const auto& arg : node.args) {
    if (arg && CallsAnything(*arg)) return true;
  }
  return false;
}

// Tags one mention of a name.
std::string Marked(const std::string& name, std::vector<std::string>* marks) {
  if (marks == nullptr) return NameToLatex(name);
  marks->push_back(name);
  return "\\sbref{" + std::to_string(marks->size()) + "}{" + NameToLatex(name) + "}";
}

std::string Bracket(const std::string& inner) {
  return "\\left(" + inner + "\\right)";
}

// Standard function names set upright, the way TeX sets \sin.
bool IsUprightOperator(const std::string& name) {
  static const char* const names[] = {"sin",  "cos",  "tan",  "sinh", "cosh", "tanh",
                                      "asin", "acos", "atan", "exp",  "log",  "ln"};
  for (const char* candidate : names) {
    if (name == candidate) return true;
  }
  return false;
}

// A product is written as a flat list so that repeated factors can become a
// power: `r * x * x` is `rx^2`, not `r x x`.
void FlattenProduct(const Node& node, std::vector<const Node*>* out) {
  if (node.kind == NodeKind::Mul) {
    FlattenProduct(*node.lhs, out);
    FlattenProduct(*node.rhs, out);
    return;
  }
  out->push_back(&node);
}

std::string Emit(const Node& node, int parentPrecedence, std::vector<std::string>* marks) {
  const int precedence = Precedence(node.kind);
  std::string out;

  switch (node.kind) {
    case NodeKind::Number:
      return node.text;
    case NodeKind::Ident:
      return Marked(node.text, marks);
    case NodeKind::Member:
      // The parts of a value are its subscripts. `p.x` is the x of p, which is
      // what a mathematician has always written as a small x under it.
      return Emit(*node.lhs, precedence, marks) + "_{" + NameToLatex(node.text) + "}";
    case NodeKind::Call: {
      // An absolute value is bars. The same bars a norm is written with, but
      // single: a number's size and a vector's length are the same idea and are
      // told apart by the doubling.
      if (node.text == "abs" && node.args.size() == 1) {
        return "\\left| " + Emit(*node.args[0], 0, marks) + " \\right|";
      }
      if (node.text == "length" && node.args.size() == 1) {
        // A shader's `length` is the norm, and a norm is written with bars.
        return "\\left\\Vert " + Emit(*node.args[0], 0, marks) + " \\right\\Vert";
      }
      // The products written as functions rather than as operators, which is how
      // a box that has to run as a shader must write them -- GLSL has no
      // operator overloading. They mean the same thing, so they read the same.
      if (node.args.size() == 2) {
        const char* infix = nullptr;
        if (node.text == "gp") infix = " ";
        if (node.text == "ip") infix = " \\cdot ";
        if (node.text == "op") infix = " \\wedge ";
        if (node.text == "rp") infix = " \\vee ";
        if (node.text == "add") infix = " + ";
        if (node.text == "sub") infix = " - ";
        if (infix != nullptr) {
          return Emit(*node.args[0], 5, marks) + infix + Emit(*node.args[1], 6, marks);
        }
      }
      if (node.args.size() == 1 && node.text == "rev") {
        return "\\widetilde{" + Emit(*node.args[0], 0, marks) + "}";
      }
      if (node.args.size() == 1 && node.text == "dual") {
        return Emit(*node.args[0], 6, marks) + "^{*}";
      }

      // Grade selection, written the way Hestenes writes it. One departure: the
      // subscript is always there, including on the scalar part -- Hestenes
      // drops the zero, and dropping it makes the most easily missed of the
      // projections the one that looks like no projection at all.
      if (node.args.size() == 1 && node.text.size() > 5 &&
          node.text.compare(0, 5, "grade") == 0 &&
          node.text.find_first_not_of("0123456789", 5) == std::string::npos) {
        return "\\langle " + Emit(*node.args[0], 0, marks) + " \\rangle_{" +
               node.text.substr(5) + "}";
      }
      if (node.text == "sqrt" && node.args.size() == 1) {
        return "\\sqrt{" + Emit(*node.args[0], 0, marks) + "}";
      }
      if (node.text == "pow" && node.args.size() == 2) {
        return Emit(*node.args[0], 4, marks) + "^{" + Emit(*node.args[1], 0, marks) + "}";
      }
      std::string arguments;
      for (std::size_t i = 0; i < node.args.size(); ++i) {
        if (i != 0) arguments += ", ";
        arguments += Emit(*node.args[i], 0, marks);
      }

      // A constructor is not a function of anything. `vec3(a, b, c)` is the
      // tuple (a, b, c) written out, and `Complex(a, b)` is a + bi -- the name
      // in front of the brackets is how the language spells a value it has no
      // notation for, and mathematics has the notation.
      if (IsVectorType(node.text) && node.args.size() > 1) return Bracket(arguments);
      if (IsComplexType(node.text) && node.args.size() == 2) {
        const std::string sum =
            Emit(*node.args[0], 4, marks) + " + " + Emit(*node.args[1], 5, marks) + "i";
        // It comes out a sum where the call was an atom, so it has to bracket
        // itself: `z^2 - Complex(a, b)` is not `z^2 - a + bi`.
        return (parentPrecedence > 4) ? Bracket(sum) : sum;
      }

      if (!IsUprightOperator(node.text)) return NameToLatex(node.text) + Bracket(arguments);

      // `\sin x`, but `\sin(1.4x)`: an operator name needs no brackets around a
      // single symbol, and a mathematician would not write any.
      const bool bare = node.args.size() == 1 && (node.args[0]->kind == NodeKind::Ident ||
                                                  node.args[0]->kind == NodeKind::Number);
      return "\\" + node.text + (bare ? (" " + arguments) : Bracket(arguments));
    }
    case NodeKind::Neg:
      out = "-" + Emit(*node.lhs, precedence, marks);
      break;
    // The geometric product is juxtaposition, so it is already what Mul emits;
    // the other two get the symbols a geometric algebra text uses.
    // **A sandwich is written out as the sandwich it is**: the versor, the
    // thing, and the versor reversed. Mathematics has no symbol for it -- what
    // `>>` is shorthand for is exactly this product -- so the shorthand is
    // spent here rather than carried through to the page.
    case NodeKind::Sandwich: {
      const int tight = Precedence(NodeKind::Mul);
      // Thin spaces, because this is three things juxtaposed and the names here
      // are words as often as letters: set tight, `R e12 ~R` runs together.
      out = Emit(*node.lhs, tight + 1, marks) + " \\, " + Emit(*node.rhs, tight + 1, marks) +
            " \\, " + "\\widetilde{" + Emit(*node.lhs, 0, marks) + "}";
      break;
    }
    case NodeKind::Inner:
      out = Emit(*node.lhs, precedence, marks) + " \\cdot " +
            Emit(*node.rhs, precedence + 1, marks);
      break;
    case NodeKind::Outer:
      out = Emit(*node.lhs, precedence, marks) + " \\wedge " +
            Emit(*node.rhs, precedence + 1, marks);
      break;
    case NodeKind::Regressive:
      out = Emit(*node.lhs, precedence, marks) + " \\vee " +
            Emit(*node.rhs, precedence + 1, marks);
      break;
    // The reverse wears a tilde and the dual a star, both on the thing itself.
    case NodeKind::Reverse:
      out = "\\widetilde{" + Emit(*node.lhs, 0, marks) + "}";
      return out;
    case NodeKind::Add:
      out = Emit(*node.lhs, precedence, marks) + " + " + Emit(*node.rhs, precedence + 1, marks);
      break;
    case NodeKind::Sub:
      out = Emit(*node.lhs, precedence, marks) + " - " + Emit(*node.rhs, precedence + 1, marks);
      break;
    case NodeKind::Div: {
      // A fraction brackets its own operands, so neither side needs parentheses.
      out = "\\frac{" + Emit(*node.lhs, 0, marks) + "}{" + Emit(*node.rhs, 0, marks) + "}";
      return out;
    }
    case NodeKind::Mul: {
      std::vector<const Node*> factors;
      FlattenProduct(node, &factors);

      std::vector<std::string> parts;
      std::vector<std::string> keys;
      parts.reserve(factors.size());
      keys.reserve(factors.size());
      for (const Node* factor : factors) {
        parts.push_back(Emit(*factor, precedence + 1, marks));
        // Compared untagged: two mentions of x are the same factor even though
        // their tags differ.
        keys.push_back(Emit(*factor, precedence + 1, nullptr));
      }

      for (std::size_t i = 0; i < parts.size();) {
        // Adjacent equal factors are one power. Only adjacent ones: reordering
        // a product is a rewrite, not a rendering.
        std::size_t j = i;
        while (j < keys.size() && keys[j] == keys[i]) ++j;
        std::string term = parts[i];
        if (j - i > 1) term += "^{" + std::to_string(j - i) + "}";

        if (!out.empty()) {
          // Mathematicians write juxtaposition, but "2 * 3" cannot become "23",
          // so a centre dot goes in whenever the next factor opens with a digit.
          const bool needsDot =
              !term.empty() && std::isdigit(static_cast<unsigned char>(term[0])) != 0;
          // And a name that is a whole word, beside anything, gets a thin space:
          // `speed time` set tight is `speedtime`, which reads as one name.
          const bool wordy = keys[i].rfind("\\mathit{", 0) == 0 ||
                             keys[i - 1].rfind("\\mathit{", 0) == 0;
          out += needsDot ? " \\cdot " : (wordy ? " \\, " : " ");
        }
        out += term;
        i = j;
      }
      break;
    }
  }

  return (precedence < parentPrecedence) ? Bracket(out) : out;
}

// The types a declaration or a parameter may be written in: the reals, the one
// function spelling, and whatever the box's algebra calls its elements.
bool IsKnownType(std::string_view text, const clifford::Signature& sig) {
  return IsRealType(text) || mathline::IsVectorType(text) || mathline::IsComplexType(text) ||
         mathline::IsColorType(text) ||
         mathline::IsMultivectorType(text, sig);
}

// `(float x) { return <expression>; }` -- everything after the name of a
// function written on one line. Reports the parameter's name and the token
// range of the body, or false if the line is not that shape.
bool MatchFunctionTail(const std::vector<Token>& tokens, std::size_t index,
                       const clifford::Signature& sig, std::string* parameterType,
                       std::string* parameter, std::size_t* bodyBegin, std::size_t* bodyEnd) {
  if (tokens[index].kind != Tok::LParen) return false;
  ++index;
  if (tokens[index].kind != Tok::Ident || !IsKnownType(tokens[index].text, sig)) return false;
  *parameterType = tokens[index].text;
  ++index;
  if (tokens[index].kind != Tok::Ident) return false;
  *parameter = tokens[index].text;
  ++index;
  if (tokens[index].kind != Tok::RParen) return false;
  ++index;

  if (tokens[index].kind != Tok::LBrace) return false;
  ++index;
  if (tokens[index].kind != Tok::Ident || tokens[index].text != "return") return false;
  ++index;

  *bodyBegin = index;
  while (tokens[index].kind != Tok::Semi) {
    if (tokens[index].kind == Tok::End) return false;
    ++index;
  }
  *bodyEnd = index;
  ++index;

  if (tokens[index].kind != Tok::RBrace) return false;
  ++index;
  if (tokens[index].kind == Tok::Semi) ++index;
  return tokens[index].kind == Tok::End;
}

// `Viz(Real);` -- the line that says this box draws that type rather than using
// one. Hand-parsed, like `Cl(...)`, because it is a statement about the box
// and never reaches a compiler.
bool ParseVizSigil(std::string_view line, std::string* type, std::string* window) {
  std::size_t at = line.find_first_not_of(" 	");
  if (at == std::string_view::npos) return false;
  if (line.compare(at, 4, "Viz(") != 0) return false;
  at += 4;

  const std::size_t begin = at;
  while (at < line.size() && (IsIdentPart(line[at]) || (at == begin && IsIdentStart(line[at])))) {
    ++at;
  }
  if (at == begin) return false;
  const std::string_view name = line.substr(begin, at - begin);

  // An optional second name: which window this viz is for.
  std::string_view where;
  while (at < line.size() && line[at] == ' ') ++at;
  if (at < line.size() && line[at] == ',') {
    ++at;
    while (at < line.size() && line[at] == ' ') ++at;
    const std::size_t from = at;
    while (at < line.size() && (IsIdentPart(line[at]) ||
                                (at == from && IsIdentStart(line[at])))) {
      ++at;
    }
    if (at == from) return false;
    where = line.substr(from, at - from);
  }

  while (at < line.size() && line[at] == ' ') ++at;
  if (at >= line.size() || line[at] != ')') return false;
  ++at;
  while (at < line.size() && line[at] == ' ') ++at;
  if (at < line.size() && line[at] == ';') ++at;
  if (line.find_first_not_of(" 	", at) != std::string_view::npos) return false;

  if (type != nullptr) *type = std::string(name);
  if (window != nullptr) *window = std::string(where);
  return true;
}

// `Samples(float, 24);` -- the line that says this box spreads that type out,
// and how thickly. Hand-parsed like the other two sigils, because it is a
// statement about the box and never reaches a compiler.
bool ParseSamplesSigil(std::string_view line, std::string* type, int* count) {
  std::size_t at = line.find_first_not_of(" 	");
  if (at == std::string_view::npos) return false;
  if (line.compare(at, 8, "Samples(") != 0) return false;
  at += 8;

  const std::size_t begin = at;
  while (at < line.size() && (IsIdentPart(line[at]) || (at == begin && IsIdentStart(line[at])))) {
    ++at;
  }
  if (at == begin) return false;
  const std::string_view name = line.substr(begin, at - begin);

  while (at < line.size() && line[at] == ' ') ++at;
  if (at >= line.size() || line[at] != ',') return false;
  ++at;
  while (at < line.size() && line[at] == ' ') ++at;
  int               many = 0;
  const std::size_t from = at;
  while (at < line.size() && std::isdigit(static_cast<unsigned char>(line[at])) != 0) {
    many = many * 10 + (line[at] - '0');
    ++at;
  }
  if (at == from) return false;

  while (at < line.size() && line[at] == ' ') ++at;
  if (at >= line.size() || line[at] != ')') return false;
  ++at;
  while (at < line.size() && line[at] == ' ') ++at;
  if (at < line.size() && line[at] == ';') ++at;
  if (line.find_first_not_of(" 	", at) != std::string_view::npos) return false;

  if (type != nullptr) *type = std::string(name);
  if (count != nullptr) *count = many;
  return true;
}

}  // namespace


// --------------------------------------------------------------------- wiring
namespace {

// The operator as it is written in the code, which is what goes in the little
// box where two wires meet. The code's spelling rather than the mathematics':
// a wiring diagram is about what the machine does.
const char* OperatorGlyph(NodeKind kind) {
  switch (kind) {
    case NodeKind::Add: return "+";
    case NodeKind::Sub: return "-";
    case NodeKind::Mul: return "*";
    case NodeKind::Div: return "/";
    case NodeKind::Neg: return "-";
    case NodeKind::Inner: return "|";
    case NodeKind::Outer: return "^";
    case NodeKind::Regressive: return "&";
    case NodeKind::Sandwich: return ">>";
    case NodeKind::Reverse: return "~";
    default: return "";
  }
}

// Flattens the parser's tree into the graph. Returns the index of the node the
// subtree feeds, or -1 for a subtree with nothing to draw -- a bare number.
int Flatten(const Node& node, Flow* flow) {
  switch (node.kind) {
    case NodeKind::Number:
      // A literal has nowhere to come from, so it contributes no wire and the
      // operator it feeds simply has one input.
      return -1;

    case NodeKind::Ident: {
      FlowNode leaf;
      leaf.name = node.text;
      flow->nodes.push_back(leaf);
      return static_cast<int>(flow->nodes.size()) - 1;
    }

    case NodeKind::Member: {
      // A part of a value is not a value of its own: there is no window for the
      // x of p, only one for p. So the wire comes from p, and reaching into it
      // is not a step in the working.
      const Node* base = &node;
      while (base->kind == NodeKind::Member && base->lhs != nullptr) base = base->lhs.get();
      if (base->kind != NodeKind::Ident) return -1;
      FlowNode leaf;
      leaf.name = base->text;
      flow->nodes.push_back(leaf);
      return static_cast<int>(flow->nodes.size()) - 1;
    }

    case NodeKind::Call: {
      // A call is one box fed by whatever its arguments are, named as it is
      // written -- `sin`, `grade0`, `length`.
      FlowNode op;
      op.op = node.text;
      std::vector<int> feeds;
      for (const NodePtr& argument : node.args) {
        if (argument != nullptr) feeds.push_back(Flatten(*argument, flow));
      }
      op.lhs = feeds.size() > 0 ? feeds[0] : -1;
      op.rhs = feeds.size() > 1 ? feeds[1] : -1;
      flow->nodes.push_back(op);
      return static_cast<int>(flow->nodes.size()) - 1;
    }

    default: {
      FlowNode op;
      op.op = OperatorGlyph(node.kind);
      if (op.op.empty()) return -1;
      const int left = (node.lhs != nullptr) ? Flatten(*node.lhs, flow) : -1;
      const int right = (node.rhs != nullptr) ? Flatten(*node.rhs, flow) : -1;
      op.lhs = left;
      op.rhs = right;
      flow->nodes.push_back(op);
      return static_cast<int>(flow->nodes.size()) - 1;
    }
  }
}

}  // namespace

Flow Wiring(std::string_view source, const clifford::Signature& sig) {
  Flow flow;

  bool tokenizeOk = false;
  const std::vector<Token> tokens = Tokenize(source, &tokenizeOk);
  if (!tokenizeOk || tokens.empty()) return flow;

  std::size_t index = 0;
  if (tokens[index].kind == Tok::Ident &&
      (IsRealType(tokens[index].text) ||
       mathline::IsMultivectorType(tokens[index].text, sig))) {
    ++index;
  }
  if (tokens[index].kind != Tok::Ident) return flow;
  const std::string name = tokens[index].text;
  ++index;
  if (tokens[index].kind != Tok::Assign) return flow;
  ++index;

  std::vector<Token> rest(tokens.begin() + static_cast<std::ptrdiff_t>(index), tokens.end());
  Parser parser(rest);
  NodePtr expression = parser.ParseExpression();
  if (!parser.ok() || expression == nullptr) return flow;
  parser.Accept(Tok::Semi);
  if (parser.Peek().kind != Tok::End) return flow;

  flow.result = name;
  flow.root = Flatten(*expression, &flow);
  return flow;
}

bool ParseFunctionHead(std::string_view source, FunctionHead* out,
                       const clifford::Signature& sig) {
  bool tokenizeOk = false;
  // A body running on past this line leaves the tokeniser with characters it
  // does not recognise, which is fine: everything the head needs is in front of
  // them, and the tokens up to that point are still good. It stops without an
  // End when that happens, so one is put back on -- everything below reads by
  // index and needs somewhere to stop.
  std::vector<Token> tokens = Tokenize(source, &tokenizeOk);
  tokens.push_back({Tok::End, {}});

  std::size_t index = 0;
  if (tokens[index].kind != Tok::Ident || !IsKnownType(tokens[index].text, sig)) return false;
  const std::string returnType = tokens[index].text;
  ++index;
  if (tokens[index].kind != Tok::Ident) return false;
  const std::string name = tokens[index].text;
  ++index;
  if (tokens[index].kind != Tok::LParen) return false;
  ++index;
  if (tokens[index].kind != Tok::Ident || !IsKnownType(tokens[index].text, sig)) return false;
  const std::string parameterType = tokens[index].text;
  ++index;
  if (tokens[index].kind != Tok::Ident) return false;
  const std::string parameter = tokens[index].text;
  ++index;
  if (tokens[index].kind != Tok::RParen) return false;
  ++index;
  const bool body = tokens[index].kind == Tok::LBrace;
  if (!body && tokens[index].kind != Tok::Semi) return false;

  if (out != nullptr) *out = {returnType, name, parameterType, parameter, body};
  return true;
}

std::vector<FunctionSpan> FindFunctions(const std::vector<std::string>& lines,
                                        const clifford::Signature& sig) {
  std::vector<FunctionSpan> spans;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    FunctionHead head;
    if (!ParseFunctionHead(lines[i], &head, sig)) continue;

    // A prototype has no body to find the end of. It is still a function --
    // the app supplies what the user did not -- so it gets a span of its own,
    // one line long, and everything that asks "which function is this line in"
    // gets the same kind of answer for it as for any other.
    if (!head.body) {
      spans.push_back({head, static_cast<int>(i), static_cast<int>(i)});
      continue;
    }

    int         depth = 0;
    std::size_t end = i;
    for (std::size_t k = i; k < lines.size(); ++k) {
      for (const char c : lines[k]) {
        if (c == '{') ++depth;
        else if (c == '}') --depth;
      }
      end = k;
      if (depth <= 0) break;
    }
    // Still open at the end of the run: it is being typed, not defined yet.
    if (depth != 0) continue;
    spans.push_back({head, static_cast<int>(i), static_cast<int>(end)});
    i = end;
  }
  return spans;
}

int SpanAt(const std::vector<FunctionSpan>& spans, int line) {
  for (std::size_t i = 0; i < spans.size(); ++i) {
    if (line >= spans[i].first && line <= spans[i].last) return static_cast<int>(i);
  }
  return -1;
}

std::string GlslTypeName(std::string_view type) {
  if (type == "double") return "float";
  return std::string(type);
}

bool IsRealType(std::string_view type) { return type == "float" || type == "double"; }

bool IsMultivectorType(std::string_view type, const clifford::Signature& sig) {
  return clifford::IsMultivectorType(type, sig);
}

bool IsVectorType(std::string_view type) {
  return type == "vec2" || type == "vec3" || type == "vec4";
}

bool IsComplexType(std::string_view type) { return type == types::complexnum::kName; }

bool IsColorType(std::string_view type) { return type == types::color::kName; }

std::string NameToLatex(std::string_view name) {
  if (name.empty()) return {};

  // Split a trailing subscript, so `x1` and `v_max` read the way they would in
  // a paper rather than as a product of letters.
  std::string base(name);
  std::string subscript;

  const std::size_t underscore = base.find('_');
  if (underscore != std::string::npos && underscore + 1 < base.size()) {
    subscript = base.substr(underscore + 1);
    base = base.substr(0, underscore);
  } else {
    std::size_t digits = base.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(base[digits - 1])) != 0) --digits;
    if (digits > 0 && digits < base.size()) {
      subscript = base.substr(digits);
      base = base.substr(0, digits);
    }
  }

  // A single letter is a variable and italicises on its own; a word would
  // otherwise be set as a product of its letters.
  std::string out = (base.size() == 1) ? base : ("\\mathit{" + base + "}");
  if (!subscript.empty()) out += "_{" + subscript + "}";
  return out;
}

namespace {

// The set a *value* of this type belongs to. A real is R, the plane and the
// space are powers of it, a complex number is C, and a multivector is in
// whichever part of the algebra its grades make up.
//
// `RToR` is a real here rather than an arrow, which looks like an exception and
// is not: `RToR w(float x)` is our spelling for a function of reals to reals,
// so what the function *gives back* is a real. The arrow is the whole
// signature, not its right-hand end.
std::string SetLatex(std::string_view type, const clifford::Signature& sig) {
  if (IsComplexType(type)) return "\\mathbb{C}";
  if (IsVectorType(type)) return "\\mathbb{R}^{" + std::string(1, type.back()) + "}";
  if (mathline::IsMultivectorType(type, sig)) return clifford::TypeLatex(type, sig);
  return "\\mathbb{R}";
}

// True if this line closes every brace it opens. It is what tells the head of a
// function whose body runs on from a whole definition written on one line --
// and the one-liner already reads as the equality it is, so only the first
// wants the arrow.
bool BracesBalance(std::string_view source) {
  int depth = 0;
  for (const char c : source) {
    if (c == '{') ++depth;
    else if (c == '}') --depth;
  }
  return depth == 0;
}

}  // namespace

Line Parse(std::string_view source, const clifford::Signature& sig, const Inside& inside) {
  Line line;

  bool tokenizeOk = false;
  const std::vector<Token> tokens = Tokenize(source, &tokenizeOk);
  if (tokens.size() == 1 && tokens[0].kind == Tok::End) return line;  // blank

  line.kind = Kind::Unparsed;
  if (!tokenizeOk) return line;

  // Saying what this box draws is a statement *about* the box rather than one
  // in it, exactly as choosing the algebra is.
  if (std::string drawn, where; ParseVizSigil(source, &drawn, &where)) {
    line.kind = Kind::Viz;
    line.latex = "\\mathrm{viz}\\;" + NameToLatex(drawn);
    // The window rides along in the name, so nothing downstream needs a second
    // field to carry it and two vizes for one type key apart on their own.
    line.name = where.empty() ? drawn : (drawn + "@" + where);
    return line;
  }

  // A set of samples is a set, and it is written as one: 24 of them, drawn out
  // of whatever space the type is.
  {
    std::string spread;
    int         many = 0;
    if (ParseSamplesSigil(source, &spread, &many)) {
      line.kind = Kind::Samples;
      line.name = spread;
      line.count = many;
      const std::string space = mathline::IsMultivectorType(spread, sig)
                                    ? clifford::TypeLatex(spread, sig)
                                    : std::string("\\mathbb{R}");
      line.latex = "S_{" + std::to_string(many) + "} \\subset " + space;
      return line;
    }
  }

  // Choosing the algebra is a statement of mathematics, not a statement in the
  // program: it names the setting everything after it is written in.
  clifford::Signature chosen{};
  if (clifford::ParseAlgebra(source, &chosen)) {
    line.kind = Kind::Algebra;
    line.latex = clifford::AlgebraLatex(chosen);
    return line;
  }

  // A function's signature says what a function *is*: an arrow from one space
  // to another. That is the whole of what the head of a definition claims --
  // what it actually does is in the lines below it, and the `return` at the end
  // is the equality that settles it.
  // A prototype always reads as the arrow; a head that opens a body reads as
  // one only when that body runs on past this line, since a whole definition
  // written on one line already reads as the equality it is.
  if (FunctionHead head;
      ParseFunctionHead(source, &head, sig) && (!head.body || !BracesBalance(source))) {
    line.kind = head.body ? Kind::Signature : Kind::Prototype;
    line.type = head.returnType;
    line.name = head.name;
    // The brace the code opens is kept, set as mathematics rather than as
    // punctuation. It is the one part of a definition's frame that says
    // something a mathematician would also want said: the arrow claims what the
    // function is, and the brace says the working for it starts here.
    // A colour has no reading as mathematics yet, so an arrow to or from one
    // would be a claim about a set nothing names: that line stays code.
    if (IsColorType(head.paramType) || IsColorType(head.returnType)) return line;
    line.latex = Marked(head.name, &line.marks) + " : " + SetLatex(head.paramType, sig) +
                 " \\to " + SetLatex(head.returnType, sig);
    // A prototype opens no brace, so it gets none -- which is the whole of what
    // it looks like on the page: an arrow, and nothing said about what travels
    // along it.
    if (head.body) line.latex += " \\;\\{";
    return line;
  }

  // Two lines mean something only inside a function, and nothing at all outside
  // one.
  if (!inside.function.empty()) {
    // What the function comes to is what the function is, so it is written as
    // the equality that defines it -- the same reading a one-line function
    // gets, set where the answer actually is.
    if (tokens[0].kind == Tok::Ident && tokens[0].text == "return") {
      std::vector<Token> rest(tokens.begin() + 1, tokens.end());
      Parser  parser(rest);
      NodePtr expression = parser.ParseExpression();
      if (!parser.ok() || expression == nullptr) return line;
      parser.Accept(Tok::Semi);
      if (parser.Peek().kind != Tok::End) return line;

      line.kind = Kind::Return;
      line.name = inside.function;
      // The argument is marked too, so pointing at a picture lights it up here
      // as well as in the working.
      const std::string head = Marked(inside.function, &line.marks) + "(" +
                               Marked(inside.parameter, &line.marks) + ")";
      const std::string shown = Emit(*expression, 0, &line.marks);
      // The head marks the function's name and its argument, so anything beyond
      // those means the answer is worked out from something rather than fixed.
      line.latex = head + Defines(line.marks.size() > 2 || CallsAnything(*expression)) + shown;
      return line;
    }

    // And the brace that closes it closes the pair. Set as mathematics, so a
    // definition reads as a definition in both halves rather than ending on a
    // stray character of C. The brace that closes a loop *inside* the function
    // is not that brace and stays code.
    if (inside.closes && tokens[0].kind == Tok::RBrace && tokens[1].kind == Tok::End) {
      line.kind = Kind::Closing;
      line.latex = "\\}";
      return line;
    }
  }

  std::size_t index = 0;
  std::string type;
  if (tokens[index].kind == Tok::Ident &&
      (IsRealType(tokens[index].text) || IsColorType(tokens[index].text) ||
       mathline::IsMultivectorType(tokens[index].text, sig))) {
    type = tokens[index].text;
    ++index;
  }
  if (tokens[index].kind != Tok::Ident) return line;
  const std::string name = tokens[index].text;
  ++index;
  // A colour is recognised -- it is a value, with a knob -- but it has no
  // reading as mathematics yet, so its line stays code.
  if (IsColorType(type)) {
    line.type = type;
    line.name = name;
    if (tokens[index].kind == Tok::Semi && tokens[index + 1].kind == Tok::End) {
      line.kind = Kind::Declaration;
      line.hasInitialiser = false;
    } else if (tokens[index].kind == Tok::Assign) {
      line.kind = Kind::Declaration;
      line.hasInitialiser = true;
    }
    return line;
  }

  // `float a;`
  if (tokens[index].kind == Tok::Semi && !type.empty()) {
    if (tokens[index + 1].kind != Tok::End) return line;
    line.kind = Kind::Declaration;
    line.type = type;
    line.name = name;
    line.hasInitialiser = false;
    // A declaration declares a *value*, and a value is an element of a set. An
    // arrow is declared by a prototype, above, which is a different shape of
    // line entirely.
    line.latex = Marked(name, &line.marks) + " \\in " + SetLatex(type, sig);
    return line;
  }

  // A function is written the way GLSL writes one, because a function *is*
  // GLSL now: `float w(float x) { return sin(x); }`. There is no `=` in
  // it, so it is looked for before one is required. Any known type may stand in
  // front of it -- a function is a shape, not a type.
  if (!type.empty() && tokens[index].kind == Tok::LParen) {
    std::string parameterType;
    std::string parameter;
    std::size_t bodyBegin = 0;
    std::size_t bodyEnd = 0;
    if (!MatchFunctionTail(tokens, index, sig, &parameterType, &parameter, &bodyBegin, &bodyEnd)) {
      return line;
    }

    std::vector<Token> body(tokens.begin() + static_cast<std::ptrdiff_t>(bodyBegin),
                            tokens.begin() + static_cast<std::ptrdiff_t>(bodyEnd));
    body.push_back({Tok::End, {}});
    Parser  bodyParser(body);
    NodePtr expression = bodyParser.ParseExpression();
    if (!bodyParser.ok() || expression == nullptr || bodyParser.Peek().kind != Tok::End) {
      return line;
    }

    line.kind = Kind::Declaration;
    line.type = type;
    line.name = name;
    line.hasInitialiser = true;
    const std::string head =
        Marked(name, &line.marks) + "(" + Marked(parameter, &line.marks) + ")";
    const std::string shown = Emit(*expression, 0, &line.marks);
    line.latex = head + Defines(line.marks.size() > 2 || CallsAnything(*expression)) + shown;
    return line;
  }

  if (tokens[index].kind != Tok::Assign) return line;
  ++index;

  std::vector<Token> rest(tokens.begin() + static_cast<std::ptrdiff_t>(index), tokens.end());
  Parser parser(rest);
  NodePtr expression = parser.ParseExpression();
  if (!parser.ok() || expression == nullptr) return line;
  // A trailing semicolon is optional; anything else after the expression is not
  // something we understand.
  parser.Accept(Tok::Semi);
  if (parser.Peek().kind != Tok::End) return line;

  line.kind = type.empty() ? Kind::Assignment : Kind::Declaration;
  line.type = type;
  line.name = name;
  line.hasInitialiser = true;
  const std::string head = Marked(name, &line.marks);
  const std::string body = Emit(*expression, 0, &line.marks);
  line.latex = head + Defines(line.marks.size() > 1 || CallsAnything(*expression)) + body;
  return line;
}

}  // namespace sb::mathline
