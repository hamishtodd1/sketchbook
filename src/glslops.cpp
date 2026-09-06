#include "glslops.h"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace sb::glslops {
namespace {

struct Token {
  enum class Kind { Ident, Number, Op, Punct, End };
  Kind        kind = Kind::End;
  std::string text;
  int         line = 0;
};

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdentPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// The multi-character operators, longest first, so that `<=` is never read as
// `<` and `&&` is never read as two algebra products.
const char* const kLongOps[] = {"<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "++", "--",
                                "+=",  "-=",  "*=", "/=", "^=", "&=", "|=", "<<", ">>", "->"};

std::vector<Token> Tokenise(const std::string& source, std::string* error) {
  std::vector<Token> out;
  int                line = 0;
  std::size_t        i = 0;

  while (i < source.size()) {
    const char c = source[i];
    if (c == '\n') {
      ++line;
      ++i;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    // Comments are carried through untouched, as one opaque token, so nothing
    // inside one is ever rewritten.
    if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
      const std::size_t begin = i;
      while (i < source.size() && source[i] != '\n') ++i;
      out.push_back({Token::Kind::Punct, source.substr(begin, i - begin), line});
      continue;
    }
    if (IsIdentStart(c)) {
      const std::size_t begin = i;
      while (i < source.size() && IsIdentPart(source[i])) ++i;
      out.push_back({Token::Kind::Ident, source.substr(begin, i - begin), line});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && i + 1 < source.size() &&
         std::isdigit(static_cast<unsigned char>(source[i + 1])))) {
      const std::size_t begin = i;
      while (i < source.size() &&
             (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '.')) {
        // An exponent's sign belongs to the number.
        if ((source[i] == 'e' || source[i] == 'E') && i + 1 < source.size() &&
            (source[i + 1] == '+' || source[i + 1] == '-')) {
          i += 2;
          continue;
        }
        ++i;
      }
      out.push_back({Token::Kind::Number, source.substr(begin, i - begin), line});
      continue;
    }

    bool matched = false;
    for (const char* op : kLongOps) {
      const std::size_t length = std::string(op).size();
      if (source.compare(i, length, op) != 0) continue;
      out.push_back({Token::Kind::Op, op, line});
      i += length;
      matched = true;
      break;
    }
    if (matched) continue;

    out.push_back({std::string("+-*/^&|~!").find(c) != std::string::npos ? Token::Kind::Op
                                                                        : Token::Kind::Punct,
                   std::string(1, c), line});
    ++i;
  }
  (void)error;
  out.push_back({Token::Kind::End, "", line});
  return out;
}

// What each operator becomes, and how tightly it binds. Higher binds tighter.
// The algebra is above `+` and `-` throughout, which is the whole departure
// from C.
int Precedence(const std::string& op) {
  if (op == "*" || op == "/") return 4;
  if (op == "^" || op == "&") return 3;
  if (op == "|") return 2;
  if (op == "+" || op == "-") return 1;
  return 0;
}

const char* CallName(const std::string& op) {
  if (op == "*") return "gp";
  if (op == "/") return "div";
  if (op == "^") return "op";
  if (op == "&") return "rp";
  if (op == "|") return "ip";
  if (op == "+") return "add";
  if (op == "-") return "sub";
  return nullptr;
}

const char* UnaryName(const std::string& op) {
  if (op == "-") return "neg";
  if (op == "~") return "rev";
  if (op == "!") return "dual";
  return nullptr;
}

// A piece of output, remembered with the line it came from so the assembled
// text keeps the shape of the text that was written.
struct Piece {
  std::string text;
  int         line = 0;
};

class Rewriter {
 public:
  explicit Rewriter(const std::vector<Token>& tokens) : tokens_(tokens) {}

  // The whole stream: runs of expression, with everything else passed through.
  std::vector<Piece> Run() {
    std::vector<Piece> out;
    while (!AtEnd()) Segment(&out, 0);
    return out;
  }

 private:
  const std::vector<Token>& tokens_;
  std::size_t               at_ = 0;

  const Token& Peek() const { return tokens_[at_]; }
  bool         AtEnd() const { return tokens_[at_].kind == Token::Kind::End; }

  // True where an expression stops and something structural begins. These are
  // never rewritten and never crossed, so nothing the rewriter does not
  // understand can be corrupted by it.
  static bool Separates(const Token& token) {
    if (token.kind == Token::Kind::Punct) {
      return token.text != "(" && token.text != ")" && token.text != "[" && token.text != "]" &&
             token.text != ".";
    }
    if (token.kind != Token::Kind::Op) return false;
    return Precedence(token.text) == 0 && UnaryName(token.text) == nullptr;
  }

  static bool CompoundAssign(const std::string& op) {
    return op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "^=" || op == "&=" ||
           op == "|=";
  }

  // One run between separators, at bracket depth `depth`.
  void Segment(std::vector<Piece>* out, int depth) {
    (void)depth;
    if (AtEnd()) return;

    if (Separates(Peek())) {
      // `a += b` has no overload to find, so it is spelled out: the left side
      // is repeated and the operation named.
      const Token separator = Peek();
      ++at_;
      if (CompoundAssign(separator.text) && !out->empty()) {
        const std::string lhs = LastOperand(out);
        const char*       name = CallName(separator.text.substr(0, 1));
        std::vector<Piece> right;
        while (!AtEnd() && !Separates(Peek())) Expression(&right, 0);
        out->push_back({" = ", separator.line});
        out->push_back({std::string(name) + "(" + lhs + ", ", separator.line});
        for (const Piece& piece : right) out->push_back(piece);
        out->push_back({")", separator.line});
        return;
      }
      out->push_back({separator.text + " ", separator.line});
      return;
    }
    Expression(out, 0);
  }

  // Everything already emitted for the current statement, which is what a
  // compound assignment has to repeat. Good enough for the names and member
  // accesses these boxes assign to.
  std::string LastOperand(std::vector<Piece>* out) const {
    std::string text;
    for (std::size_t i = out->size(); i-- > 0;) {
      const std::string& piece = (*out)[i].text;
      // Anything structural ends the statement, not just a semicolon: a line
      // after a closing brace would otherwise drag the brace in with it.
      if (piece.find_first_of(";{}") != std::string::npos) break;
      text = piece + text;
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    return text;
  }

  // Precedence climbing. `least` is the loosest binding this call will take.
  void Expression(std::vector<Piece>* out, int least) {
    std::vector<Piece> left;
    Unary(&left);

    while (!AtEnd() && Peek().kind == Token::Kind::Op) {
      const std::string op = Peek().text;
      const int         precedence = Precedence(op);
      if (precedence == 0 || precedence < least) break;
      const char* name = CallName(op);
      if (name == nullptr) break;
      const int line = Peek().line;
      ++at_;

      std::vector<Piece> right;
      Expression(&right, precedence + 1);

      std::vector<Piece> joined;
      joined.push_back({std::string(name) + "(", line});
      for (const Piece& piece : left) joined.push_back(piece);
      joined.push_back({", ", line});
      for (const Piece& piece : right) joined.push_back(piece);
      joined.push_back({")", line});
      left = std::move(joined);
    }
    for (const Piece& piece : left) out->push_back(piece);
  }

  void Unary(std::vector<Piece>* out) {
    if (!AtEnd() && Peek().kind == Token::Kind::Op) {
      const char* name = UnaryName(Peek().text);
      // `-` is only a negation where nothing could have been subtracted.
      if (name != nullptr) {
        const int line = Peek().line;
        ++at_;
        std::vector<Piece> inner;
        Unary(&inner);
        out->push_back({std::string(name) + "(", line});
        for (const Piece& piece : inner) out->push_back(piece);
        out->push_back({")", line});
        return;
      }
    }
    Operand(out);
  }

  // A name, a number, a parenthesised expression, and whatever calls,
  // subscripts and member accesses follow it.
  void Operand(std::vector<Piece>* out) {
    if (AtEnd()) return;
    const Token& token = Peek();

    if (token.kind == Token::Kind::Punct && token.text == "(") {
      ++at_;
      out->push_back({"(", token.line});
      Group(out, ")");
      return;
    }
    if (token.kind == Token::Kind::Ident || token.kind == Token::Kind::Number) {
      out->push_back({token.text, token.line});
      ++at_;
      Suffix(out);
      return;
    }
    // Anything else is passed through untouched rather than guessed at.
    out->push_back({token.text + " ", token.line});
    ++at_;
  }

  void Suffix(std::vector<Piece>* out) {
    while (!AtEnd()) {
      const Token& token = Peek();
      if (token.kind == Token::Kind::Punct && token.text == "(") {
        ++at_;
        out->push_back({"(", token.line});
        Group(out, ")");
        continue;
      }
      if (token.kind == Token::Kind::Punct && token.text == "[") {
        ++at_;
        out->push_back({"[", token.line});
        Group(out, "]");
        continue;
      }
      if (token.kind == Token::Kind::Punct && token.text == ".") {
        ++at_;
        out->push_back({".", token.line});
        if (!AtEnd() && Peek().kind == Token::Kind::Ident) {
          out->push_back({Peek().text, Peek().line});
          ++at_;
        }
        continue;
      }
      break;
    }
  }

  // Inside brackets: expressions separated by commas, each rewritten on its
  // own, until the closing bracket.
  void Group(std::vector<Piece>* out, const char* close) {
    while (!AtEnd()) {
      if (Peek().kind == Token::Kind::Punct && Peek().text == close) {
        out->push_back({close, Peek().line});
        ++at_;
        return;
      }
      if (Separates(Peek())) {
        const Token separator = Peek();
        out->push_back({separator.text + " ", separator.line});
        ++at_;
        continue;
      }
      Expression(out, 0);
    }
  }
};

}  // namespace

bool Rewrite(const std::string& source, std::string* out, std::string* error) {
  std::string ignored;
  const std::vector<Token> tokens = Tokenise(source, error != nullptr ? error : &ignored);

  Rewriter rewriter(tokens);
  const std::vector<Piece> pieces = rewriter.Run();

  // Back into text, keeping every line break where it was: a diagnostic from
  // the driver has to land on the line the user actually wrote.
  const int lines = tokens.empty() ? 0 : tokens.back().line;
  std::string text;
  int         line = 0;
  for (const Piece& piece : pieces) {
    while (line < piece.line) {
      text += '\n';
      ++line;
    }
    // Two words that were separated by space in the source must stay separated
    // here: `float m` run together is one identifier, not a declaration.
    if (!text.empty() && !piece.text.empty() && IsIdentPart(text.back()) &&
        IsIdentPart(piece.text.front())) {
      text += ' ';
    }
    text += piece.text;
  }
  while (line < lines) {
    text += '\n';
    ++line;
  }
  if (out != nullptr) *out = text;
  return true;
}

std::string BuiltinOverloads() {
  std::ostringstream out;
  out << "// The operators GLSL already has, under the names the rewrite uses.\n";

  struct Binary {
    const char* name;
    const char* op;
  };
  const Binary kBinaries[] = {{"add", "+"}, {"sub", "-"}, {"gp", "*"}, {"div", "/"}};

  const char* const kScalars[] = {"float", "int"};
  const char* const kVectors[] = {"vec2", "vec3", "vec4"};
  const char* const kMatrices[] = {"mat2", "mat3", "mat4"};

  for (const Binary& binary : kBinaries) {
    // Scalars, including the mixed pairs -- without those, `f * i` is ambiguous
    // between the all-float and all-int overloads rather than converting.
    for (const char* a : kScalars) {
      for (const char* b : kScalars) {
        const char* result =
            (std::string(a) == "int" && std::string(b) == "int") ? "int" : "float";
        out << result << " " << binary.name << "(" << a << " a, " << b << " b) { return a "
            << binary.op << " b; }\n";
      }
    }
    // Vectors with themselves and with a scalar, either way round.
    for (const char* v : kVectors) {
      out << v << " " << binary.name << "(" << v << " a, " << v << " b) { return a " << binary.op
          << " b; }\n";
      out << v << " " << binary.name << "(" << v << " a, float b) { return a " << binary.op
          << " b; }\n";
      out << v << " " << binary.name << "(float a, " << v << " b) { return a " << binary.op
          << " b; }\n";
    }
    // Matrices likewise. `*` between two matrices is the matrix product, which
    // is what an algebra wants of it anyway.
    for (const char* m : kMatrices) {
      out << m << " " << binary.name << "(" << m << " a, " << m << " b) { return a " << binary.op
          << " b; }\n";
      out << m << " " << binary.name << "(" << m << " a, float b) { return a " << binary.op
          << " b; }\n";
      out << m << " " << binary.name << "(float a, " << m << " b) { return a " << binary.op
          << " b; }\n";
    }
  }

  // Matrix times vector, which only the geometric product spells.
  for (int i = 0; i < 3; ++i) {
    out << kVectors[i] << " gp(" << kMatrices[i] << " a, " << kVectors[i]
        << " b) { return a * b; }\n";
    out << kVectors[i] << " gp(" << kVectors[i] << " a, " << kMatrices[i]
        << " b) { return a * b; }\n";
  }

  // And the unary ones. `rev` and `dual` mean nothing for a builtin type, so
  // they are not written for one: asking for the dual of a float should say so
  // rather than quietly doing nothing.
  out << "float neg(float a) { return -a; }\n";
  out << "int neg(int a) { return -a; }\n";
  for (const char* v : kVectors) out << v << " neg(" << v << " a) { return -a; }\n";
  for (const char* m : kMatrices) out << m << " neg(" << m << " a) { return -a; }\n";

  return out.str();
}

}  // namespace sb::glslops
