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
  LBracket, RBracket, LBrace, RBrace, Comma, Assign, Semi, Amp,
  // The geometric algebra operators. They are ordinary C++ operators; a
  // Clifford algebra is what gives them these meanings.
  Bar, Caret, Tilde, Bang
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
      case '|': kind = Tok::Bar; break;
      case '^': kind = Tok::Caret; break;
      case '~': kind = Tok::Tilde; break;
      case '!': kind = Tok::Bang; break;
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
enum class NodeKind {
  Number, Ident, Call, Add, Sub, Mul, Div, Neg,
  // Inner, outer and regressive products, and the two unary ones. Written the
  // way a geometric algebra text writes them: a dot, a wedge, a vee, a tilde
  // over the thing and a star after it.
  Inner, Outer, Regressive, Reverse, Dual
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
    case NodeKind::Inner: return 1;
    case NodeKind::Outer: return 2;
    case NodeKind::Regressive: return 3;
    case NodeKind::Add:
    case NodeKind::Sub: return 4;
    case NodeKind::Mul:
    case NodeKind::Div: return 5;
    case NodeKind::Neg:
    case NodeKind::Reverse:
    case NodeKind::Dual: return 6;
    default: return 7;  // atoms
  }
}

class Parser {
 public:
  explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

  NodePtr ParseExpression() { return ParseInner(); }
  bool    ok() const { return ok_; }
  const Token& Peek() const { return tokens_[pos_]; }
  void    Advance() { if (tokens_[pos_].kind != Tok::End) ++pos_; }
  bool    Accept(Tok kind) {
    if (tokens_[pos_].kind != kind) return false;
    Advance();
    return true;
  }

 private:
  // C++ binds | looser than ^, ^ looser than &, and & looser than + -. That is
  // why `a + b | c` means `(a + b) | c` and why the brackets are worth writing.
  // Following C++ here rather than the convention of geometric algebra texts is
  // the only honest choice: the compiler is what will actually run it.
  NodePtr ParseInner() {
    NodePtr node = ParseOuter();
    while (ok_ && Peek().kind == Tok::Bar) {
      Advance();
      node = Combine(NodeKind::Inner, std::move(node), ParseOuter());
    }
    return node;
  }

  NodePtr ParseOuter() {
    NodePtr node = ParseRegressive();
    while (ok_ && Peek().kind == Tok::Caret) {
      Advance();
      node = Combine(NodeKind::Outer, std::move(node), ParseRegressive());
    }
    return node;
  }

  NodePtr ParseRegressive() {
    NodePtr node = ParseSum();
    while (ok_ && Peek().kind == Tok::Amp) {
      Advance();
      node = Combine(NodeKind::Regressive, std::move(node), ParseSum());
    }
    return node;
  }

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
    if (Peek().kind == Tok::Tilde || Peek().kind == Tok::Bang) {
      const NodeKind kind = (Peek().kind == Tok::Tilde) ? NodeKind::Reverse : NodeKind::Dual;
      Advance();
      auto node = std::make_unique<Node>();
      node->kind = kind;
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
    case NodeKind::Dual:
      out = Emit(*node.lhs, precedence, marks) + "^{*}";
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

// `Viz(Real);` -- the line that says this box draws that type rather than using
// one. Hand-parsed, like `Algebra(...)`, because it is a statement about the box
// and never reaches a compiler.
bool ParseVizSigil(std::string_view line, std::string* type) {
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

  while (at < line.size() && line[at] == ' ') ++at;
  if (at >= line.size() || line[at] != ')') return false;
  ++at;
  while (at < line.size() && line[at] == ' ') ++at;
  if (at < line.size() && line[at] == ';') ++at;
  if (line.find_first_not_of(" 	", at) != std::string_view::npos) return false;

  if (type != nullptr) *type = std::string(name);
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
    case NodeKind::Reverse: return "~";
    case NodeKind::Dual: return "!";
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
      (IsRealType(tokens[index].text) || IsFunctionType(tokens[index].text) ||
       mathline::IsMultivectorType(tokens[index].text, sig))) {
    ++index;
  }
  if (tokens[index].kind != Tok::Ident) return flow;
  const std::string name = tokens[index].text;
  ++index;
  if (tokens[index].kind != Tok::Assign) return flow;
  ++index;

  // A lambda defines a function pointwise rather than out of other values, so
  // there is nothing to wire it to.
  std::string parameter;
  std::size_t bodyBegin = 0;
  std::size_t bodyEnd = 0;
  if (MatchLambda(tokens, index, &parameter, &bodyBegin, &bodyEnd)) return flow;

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

bool IsRealType(std::string_view type) { return type == "float" || type == "double"; }

bool IsMultivectorType(std::string_view type, const clifford::Signature& sig) {
  return clifford::IsMultivectorType(type, sig);
}

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

Line Parse(std::string_view source, const clifford::Signature& sig) {
  Line line;

  bool tokenizeOk = false;
  const std::vector<Token> tokens = Tokenize(source, &tokenizeOk);
  if (tokens.size() == 1 && tokens[0].kind == Tok::End) return line;  // blank

  line.kind = Kind::Unparsed;
  if (!tokenizeOk) return line;

  // Saying what this box draws is a statement *about* the box rather than one
  // in it, exactly as choosing the algebra is.
  if (std::string drawn; ParseVizSigil(source, &drawn)) {
    line.kind = Kind::Viz;
    line.name = std::move(drawn);
    line.latex = "\\mathrm{viz}\\;" + NameToLatex(line.name);
    return line;
  }

  // Choosing the algebra is a statement of mathematics, not a statement in the
  // program: it names the setting everything after it is written in.
  clifford::Signature chosen{};
  if (clifford::ParseAlgebra(source, &chosen)) {
    line.kind = Kind::Algebra;
    line.latex = clifford::AlgebraLatex(chosen);
    return line;
  }

  std::size_t index = 0;
  std::string type;
  if (tokens[index].kind == Tok::Ident &&
      (IsRealType(tokens[index].text) || IsFunctionType(tokens[index].text) ||
       mathline::IsMultivectorType(tokens[index].text, sig))) {
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
    // A real is an element of a set, a function is an arrow between two, and a
    // multivector is an element of whichever part of the algebra it belongs to.
    if (IsFunctionType(type)) {
      line.latex = Marked(name, &line.marks) + " : \\mathbb{R} \\to \\mathbb{R}";
    } else if (mathline::IsMultivectorType(type, sig)) {
      line.latex =
          Marked(name, &line.marks) + " \\in " + clifford::TypeLatex(type, sig);
    } else {
      line.latex = Marked(name, &line.marks) + " \\in \\mathbb{R}";
    }
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
