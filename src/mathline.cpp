#include "mathline.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace sb::mathline {
namespace {

// ------------------------------------------------------------------- tokens
enum class Tok {
  End, Number, Ident, Plus, Minus, Star, Slash, LParen, RParen,
  LBracket, RBracket, LBrace, RBrace, Comma, Assign, Semi, Amp
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
    if (std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.') {
      const std::size_t start = i;
      while (i < source.size() &&
             (std::isdigit(static_cast<unsigned char>(source[i])) != 0 || source[i] == '.')) {
        ++i;
      }
      // A trailing float suffix is C++ noise, not mathematics.
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

    Tok kind = Tok::End;
    switch (c) {
      case '+': kind = Tok::Plus; break;
      case '-': kind = Tok::Minus; break;
      case '*': kind = Tok::Star; break;
      case '/': kind = Tok::Slash; break;
      case '(': kind = Tok::LParen; break;
      case ')': kind = Tok::RParen; break;
      case '[': kind = Tok::LBracket; break;
      case ']': kind = Tok::RBracket; break;
      case '{': kind = Tok::LBrace; break;
      case '}': kind = Tok::RBrace; break;
      case ',': kind = Tok::Comma; break;
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
enum class NodeKind { Number, Ident, Call, Add, Sub, Mul, Div, Neg };

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
    case NodeKind::Add:
    case NodeKind::Sub: return 1;
    case NodeKind::Mul:
    case NodeKind::Div: return 2;
    case NodeKind::Neg: return 3;
    default: return 4;  // atoms
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
  NodePtr ParseSum() {
    NodePtr node = ParseProduct();
    while (ok_ && (Peek().kind == Tok::Plus || Peek().kind == Tok::Minus)) {
      const NodeKind kind = (Peek().kind == Tok::Plus) ? NodeKind::Add : NodeKind::Sub;
      Advance();
      NodePtr rhs = ParseProduct();
      node = Combine(kind, std::move(node), std::move(rhs));
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
        return node;
      }
      // `sin(x)`, `pow(x, 2)`, or a call of the user's own function.
      Advance();
      node->kind = NodeKind::Call;
      if (Peek().kind != Tok::RParen) {
        node->args.push_back(ParseSum());
        while (ok_ && Accept(Tok::Comma)) node->args.push_back(ParseSum());
      }
      if (!Accept(Tok::RParen)) ok_ = false;
      return node;
    }
    if (Accept(Tok::LParen)) {
      NodePtr inner = ParseSum();
      if (!Accept(Tok::RParen)) ok_ = false;
      return inner;
    }
    ok_ = false;
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
    case NodeKind::Call: {
      if (node.text == "length" && node.args.size() == 1) {
        // A shader's `length` is the norm, and a norm is written with bars.
        return "\\left\\Vert " + Emit(*node.args[0], 0, marks) + " \\right\\Vert";
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
          out += needsDot ? " \\cdot " : " ";
        }
        out += term;
        i = j;
      }
      break;
    }
  }

  return (precedence < parentPrecedence) ? Bracket(out) : out;
}

// `[](float x) { return <expr>; }` -- a whole initialiser, which is the only
// place a function is written out in full. Reports the parameter's name and the
// token range of the body, or false if the line is not that shape.
bool MatchLambda(const std::vector<Token>& tokens, std::size_t index, std::string* parameter,
                 std::size_t* bodyBegin, std::size_t* bodyEnd) {
  if (tokens[index].kind != Tok::LBracket) return false;
  ++index;
  // Whatever the capture list says is C++ bookkeeping, not mathematics.
  while (tokens[index].kind != Tok::RBracket) {
    if (tokens[index].kind == Tok::End) return false;
    ++index;
  }
  ++index;

  if (tokens[index].kind != Tok::LParen) return false;
  ++index;
  if (tokens[index].kind != Tok::Ident || !IsRealType(tokens[index].text)) return false;
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

}  // namespace

bool IsRealType(std::string_view type) { return type == "float" || type == "double"; }

bool IsFunctionType(std::string_view type) { return type == "RealFunction"; }

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

Line Parse(std::string_view source) {
  Line line;

  bool tokenizeOk = false;
  const std::vector<Token> tokens = Tokenize(source, &tokenizeOk);
  if (tokens.size() == 1 && tokens[0].kind == Tok::End) return line;  // blank

  line.kind = Kind::Unparsed;
  if (!tokenizeOk) return line;

  std::size_t index = 0;
  std::string type;
  if (tokens[index].kind == Tok::Ident &&
      (IsRealType(tokens[index].text) || IsFunctionType(tokens[index].text))) {
    type = tokens[index].text;
    ++index;
  }
  if (tokens[index].kind != Tok::Ident) return line;
  const std::string name = tokens[index].text;
  ++index;

  // `float a;`
  if (tokens[index].kind == Tok::Semi && !type.empty()) {
    if (tokens[index + 1].kind != Tok::End) return line;
    line.kind = Kind::Declaration;
    line.type = type;
    line.name = name;
    line.hasInitialiser = false;
    // A real is an element of a set; a function is an arrow between two.
    line.latex = IsFunctionType(type)
                     ? (Marked(name, &line.marks) + " : \\mathbb{R} \\to \\mathbb{R}")
                     : (Marked(name, &line.marks) + " \\in \\mathbb{R}");
    return line;
  }

  if (tokens[index].kind != Tok::Assign) return line;
  ++index;

  // A function written out in full reads the way one is defined: f(x) = ...
  std::string parameter;
  std::size_t bodyBegin = 0;
  std::size_t bodyEnd = 0;
  if (MatchLambda(tokens, index, &parameter, &bodyBegin, &bodyEnd)) {
    std::vector<Token> body(tokens.begin() + static_cast<std::ptrdiff_t>(bodyBegin),
                            tokens.begin() + static_cast<std::ptrdiff_t>(bodyEnd));
    body.push_back({Tok::End, {}});
    Parser bodyParser(body);
    NodePtr expression = bodyParser.ParseExpression();
    if (!bodyParser.ok() || expression == nullptr || bodyParser.Peek().kind != Tok::End) {
      return line;
    }

    line.kind = type.empty() ? Kind::Assignment : Kind::Declaration;
    line.type = type;
    line.name = name;
    line.hasInitialiser = true;
    const std::string head = Marked(name, &line.marks) + "(" + NameToLatex(parameter) + ")";
    const std::string body2 = Emit(*expression, 0, &line.marks);
    // Everything the body mentions is now marked; anything beyond the name
    // itself means the definition depends on something.
    line.latex = head + Defines(line.marks.size() > 1) + body2;
    return line;
  }

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
  line.latex = head + Defines(line.marks.size() > 1) + body;
  return line;
}

}  // namespace sb::mathline
