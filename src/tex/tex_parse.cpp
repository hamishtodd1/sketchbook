// Recursive-descent parser for a LaTeX math fragment.
//
// Errors are recorded rather than thrown: an unknown command is rendered as
// upright literal text and the first problem is reported through Parsed::error,
// so a typo shows you where it is instead of blanking the whole formula.
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <cstdlib>

#include "tex/tex.h"

namespace sb::tex {
namespace {

constexpr int kStopBrace = 1 << 0;  // stop at an unmatched '}'
constexpr int kStopRight = 1 << 1;  // stop at \right
constexpr int kStopCell  = 1 << 2;  // stop at '&', '\\' or \end

NodePtr MakeRow() {
  auto n = std::make_unique<Node>();
  n->kind = NodeKind::Row;
  return n;
}

NodePtr MakeSymbol(const SymbolDef& def) {
  auto n = std::make_unique<Node>();
  n->kind = NodeKind::Symbol;
  n->codepoint = def.codepoint;
  n->face = def.face;
  n->atom = def.atom;
  return n;
}

NodePtr MakeSpace(float em) {
  auto n = std::make_unique<Node>();
  n->kind = NodeKind::Space;
  n->amount = em;
  return n;
}

// Rewrites the alphabet of every Symbol in a subtree, for the \math* switches.
void ApplyFace(Node* node, Face face) {
  if (node == nullptr) return;
  if (node->kind == NodeKind::Symbol) {
    const int cp = node->codepoint;
    const bool isLetter = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
    const bool isDigit = (cp >= '0' && cp <= '9');
    if (isLetter || isDigit) {
      // Caligraphic and blackboard bold only cover uppercase Latin; leave
      // anything else alone rather than substituting a glyph the face lacks.
      const bool uppercaseOnly = (face == Face::Caligraphic || face == Face::AMS);
      if (uppercaseOnly && !(cp >= 'A' && cp <= 'Z')) {
        node->face = Face::MathItalic;
      } else {
        node->face = face;
      }
    }
  }
  for (auto& kid : node->kids) ApplyFace(kid.get(), face);
  ApplyFace(node->sup.get(), face);
  ApplyFace(node->sub.get(), face);
  ApplyFace(node->index.get(), face);
  for (auto& row : node->rows) {
    for (auto& cell : row) ApplyFace(cell.get(), face);
  }
}

class Parser {
 public:
  explicit Parser(std::string_view src) : src_(src) {}

  Parsed Run() {
    Parsed out;
    out.root = ParseRow(0);
    if (!AtEnd() && error_.empty()) error_ = "unexpected '}'";
    out.error = error_;
    return out;
  }

 private:
  // ---------------------------------------------------------------- scanning
  bool AtEnd() const { return pos_ >= src_.size(); }

  void SkipSpace() {
    while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                                  src_[pos_] == '\n' || src_[pos_] == '\r')) {
      ++pos_;
    }
  }

  char PeekRaw() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }

  // Decodes one UTF-8 scalar so a formula can contain literal Greek.
  int NextCodepoint() {
    const unsigned char c = static_cast<unsigned char>(src_[pos_]);
    int cp = c;
    int extra = 0;
    if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
    else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
    else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
    ++pos_;
    for (int i = 0; i < extra && pos_ < src_.size(); ++i) {
      cp = (cp << 6) | (static_cast<unsigned char>(src_[pos_]) & 0x3F);
      ++pos_;
    }
    return cp;
  }

  // Reads a command name after the backslash has been consumed.
  std::string ReadCommandName() {
    if (AtEnd()) return {};
    const char c = src_[pos_];
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!alpha) { ++pos_; return std::string(1, c); }
    const std::size_t start = pos_;
    while (pos_ < src_.size()) {
      const char d = src_[pos_];
      if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z')) ++pos_;
      else break;
    }
    return std::string(src_.substr(start, pos_ - start));
  }

  // Peeks the next command without consuming it.
  bool PeekCommand(std::string* name) const {
    std::size_t p = pos_;
    while (p < src_.size() && (src_[p] == ' ' || src_[p] == '\t' || src_[p] == '\n' ||
                              src_[p] == '\r')) {
      ++p;
    }
    if (p >= src_.size() || src_[p] != '\\') return false;
    ++p;
    if (p >= src_.size()) return false;
    const char c = src_[p];
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!alpha) { *name = std::string(1, c); return true; }
    const std::size_t start = p;
    while (p < src_.size()) {
      const char d = src_[p];
      if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z')) ++p;
      else break;
    }
    *name = std::string(src_.substr(start, p - start));
    return true;
  }

  void Fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
  }

  // ------------------------------------------------------------------ pieces
  // Reads the raw contents of a brace group, for \text and \operatorname.
  std::string ReadBracedText() {
    SkipSpace();
    if (PeekRaw() != '{') {
      // A single token argument, as LaTeX allows.
      if (AtEnd()) return {};
      const std::size_t start = pos_;
      NextCodepoint();
      return std::string(src_.substr(start, pos_ - start));
    }
    ++pos_;  // '{'
    const std::size_t start = pos_;
    int depth = 1;
    while (pos_ < src_.size() && depth > 0) {
      const char c = src_[pos_];
      if (c == '\\' && pos_ + 1 < src_.size()) { pos_ += 2; continue; }
      if (c == '{') ++depth;
      if (c == '}') { --depth; if (depth == 0) break; }
      ++pos_;
    }
    const std::string text(src_.substr(start, pos_ - start));
    if (pos_ < src_.size()) ++pos_;  // closing '}'
    else Fail("missing '}'");
    return text;
  }

  // One mandatory argument: a brace group, or the single token that follows.
  NodePtr ParseArgument() {
    SkipSpace();
    if (PeekRaw() == '{') {
      ++pos_;
      NodePtr body = ParseRow(kStopBrace);
      if (PeekRaw() == '}') ++pos_;
      else Fail("missing '}'");
      return body;
    }
    return ParseUnit();
  }

  // Turns literal text into upright roman glyphs -- or into the bold or italic
  // cut of the same face, which is what a text box's controls select.
  NodePtr TextRow(const std::string& text, Atom atom, Face face = Face::MainRegular) {
    NodePtr row = MakeRow();
    row->atom = atom;
    std::size_t i = 0;
    while (i < text.size()) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      int cp = c;
      int extra = 0;
      if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
      else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
      else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
      ++i;
      for (int k = 0; k < extra && i < text.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
        ++i;
      }
      SymbolDef def{cp, face, Atom::Ord};
      row->kids.push_back(MakeSymbol(def));
    }
    return row;
  }

  NodePtr ParseEnvironment() {
    const std::string name = ReadBracedText();
    auto node = std::make_unique<Node>();
    node->kind = NodeKind::Array;
    node->atom = Atom::Inner;

    if (name == "pmatrix")      { node->arrayLeft = 0x28; node->arrayRight = 0x29; }
    else if (name == "bmatrix") { node->arrayLeft = 0x5B; node->arrayRight = 0x5D; }
    else if (name == "Bmatrix") { node->arrayLeft = 0x7B; node->arrayRight = 0x7D; }
    else if (name == "vmatrix") { node->arrayLeft = 0x2223; node->arrayRight = 0x2223; }
    else if (name == "Vmatrix") { node->arrayLeft = 0x2225; node->arrayRight = 0x2225; }
    else if (name == "cases")   { node->arrayLeft = 0x7B; node->arrayRight = 0; node->arrayIsCases = true; }
    else if (name == "matrix" || name == "aligned" || name == "array") { /* no fences */ }
    else Fail("unknown environment: " + name);

    node->rows.emplace_back();
    while (true) {
      SkipSpace();
      if (AtEnd()) { Fail("missing \\end{" + name + "}"); break; }

      NodePtr cell = ParseRow(kStopCell);
      node->rows.back().push_back(std::move(cell));

      SkipSpace();
      if (PeekRaw() == '&') { ++pos_; continue; }

      std::string cmd;
      if (PeekCommand(&cmd)) {
        if (cmd == "\\") {
          SkipSpace();
          pos_ += 2;  // consume the backslash pair
          node->rows.emplace_back();
          continue;
        }
        if (cmd == "end") {
          SkipSpace();
          pos_ += 1 + cmd.size();  // consume "\end"
          const std::string closing = ReadBracedText();
          if (closing != name) Fail("\\end{" + closing + "} does not match \\begin{" + name + "}");
          break;
        }
      }
      Fail("unterminated environment: " + name);
      break;
    }

    // A trailing \\ leaves an empty row behind; drop it.
    if (!node->rows.empty() && node->rows.back().size() == 1 &&
        node->rows.back()[0]->kids.empty()) {
      node->rows.pop_back();
    }
    return node;
  }

  NodePtr ParseCommand(int stopFlags) {
    const std::string name = ReadCommandName();

    // ---- explicit spacing --------------------------------------------------
    if (name == ",") return MakeSpace(3.0f / 18.0f);
    if (name == ":") return MakeSpace(4.0f / 18.0f);
    if (name == ";") return MakeSpace(5.0f / 18.0f);
    if (name == "!") return MakeSpace(-3.0f / 18.0f);
    if (name == " ") return MakeSpace(6.0f / 18.0f);
    if (name == "quad") return MakeSpace(1.0f);
    if (name == "qquad") return MakeSpace(2.0f);

    // ---- literal escapes ---------------------------------------------------
    if (name == "%" || name == "$" || name == "&" || name == "#" || name == "_") {
      SymbolDef def{static_cast<int>(name[0]), Face::MainRegular, Atom::Ord};
      return MakeSymbol(def);
    }

    // ---- style switches: they govern the rest of the enclosing list ---------
    auto styleSwitch = [&](Style s) {
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::Styled;
      node->style = s;
      node->kids.push_back(ParseRow(stopFlags));
      return node;
    };
    if (name == "displaystyle") return styleSwitch(Style::Display);
    if (name == "textstyle") return styleSwitch(Style::Text);
    if (name == "scriptstyle") return styleSwitch(Style::Script);
    if (name == "scriptscriptstyle") return styleSwitch(Style::ScriptScript);

    // ---- alphabet switches -------------------------------------------------
    auto fontSwitch = [&](Face face) {
      NodePtr body = ParseArgument();
      ApplyFace(body.get(), face);
      return body;
    };
    if (name == "mathrm" || name == "rm") return fontSwitch(Face::MainRegular);
    if (name == "mathbf" || name == "bf") return fontSwitch(Face::MainBold);
    if (name == "mathit" || name == "it") return fontSwitch(Face::MainItalic);
    // \mathscr is script, and the KaTeX distribution has no separate script
    // face -- the same one KaTeX itself uses for both.
    if (name == "mathcal" || name == "cal" || name == "mathscr") {
      return fontSwitch(Face::Caligraphic);
    }
    if (name == "mathtt" || name == "tt") return fontSwitch(Face::Mono);
    // Blackboard bold lives in the AMS face, whose A-Z *are* the double-struck
    // letters -- the same trick KaTeX uses. Lowercase has no such form, and
    // ApplyFace leaves anything the face lacks alone.
    if (name == "mathbb") return fontSwitch(Face::AMS);

    // ---- text --------------------------------------------------------------
    if (name == "text" || name == "textrm" || name == "mbox") {
      return TextRow(ReadBracedText(), Atom::Ord);
    }
    // Prose, set bold or italic. There is no bold-italic cut of Computer
    // Modern in the KaTeX set, so asking for both gives bold.
    if (name == "textbf") return TextRow(ReadBracedText(), Atom::Ord, Face::MainBold);
    if (name == "textit" || name == "emph") {
      return TextRow(ReadBracedText(), Atom::Ord, Face::MainItalic);
    }
    if (name == "operatorname") return TextRow(ReadBracedText(), Atom::Op);

    // \sbref{n}{...} tags a subtree so the app can find where it landed. It
    // typesets as exactly its argument: the tag has no effect on the output.
    if (name == "sbref") {
      const std::string tag = ReadBracedText();
      NodePtr body = ParseArgument();
      if (body != nullptr) body->mark = std::atoi(tag.c_str());
      return body;
    }

    // ---- fractions ---------------------------------------------------------
    if (name == "frac" || name == "dfrac" || name == "tfrac" || name == "binom") {
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::Fraction;
      node->atom = Atom::Inner;
      node->hasRule = (name != "binom");
      node->kids.push_back(ParseArgument());
      node->kids.push_back(ParseArgument());
      if (name == "dfrac") { node->style = Style::Display; node->styleIsExplicit = true; }
      else if (name == "tfrac") { node->style = Style::Text; node->styleIsExplicit = true; }
      else if (name == "binom") { node->leftDelim = 0x28; node->rightDelim = 0x29; }
      return node;
    }

    // ---- radicals ----------------------------------------------------------
    if (name == "sqrt") {
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::Radical;
      node->atom = Atom::Ord;
      SkipSpace();
      if (PeekRaw() == '[') {
        ++pos_;
        node->index = ParseRow(0, /*stopAtBracket=*/true);
        if (PeekRaw() == ']') ++pos_;
        else Fail("missing ']'");
      }
      node->kids.push_back(ParseArgument());
      return node;
    }

    // ---- over/underline ----------------------------------------------------
    if (name == "overline") {
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::Overline;
      node->kids.push_back(ParseArgument());
      return node;
    }
    if (name == "underline") {
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::Underline;
      node->kids.push_back(ParseArgument());
      return node;
    }

    // ---- accents -----------------------------------------------------------
    if (const SymbolDef* acc = FindAccent(name)) {
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::Accent;
      node->codepoint = acc->codepoint;
      node->face = acc->face;
      // \widehat has the same ladder in the same fonts and would take the same
      // path, but nothing writes one yet and it has not been looked at.
      node->wide = (name == "widetilde");
      node->kids.push_back(ParseArgument());
      return node;
    }

    // ---- \not and the two glyphs no face provides --------------------------
    if (name == "not" || name == "neq" || name == "ne" || name == "notin") {
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::Negated;
      if (name == "not") {
        node->kids.push_back(ParseUnit());
      } else if (name == "notin") {
        node->kids.push_back(MakeSymbol(*FindSymbol("in")));
      } else {
        node->kids.push_back(MakeSymbol({0x3D, Face::MainRegular, Atom::Rel}));
      }
      node->atom = node->kids[0]->atom;
      return node;
    }

    // ---- \left ... \right --------------------------------------------------
    if (name == "left") {
      SkipSpace();
      const int left = ReadDelimiter();
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::Delimited;
      node->atom = Atom::Inner;
      node->leftDelim = left;
      node->kids.push_back(ParseRow(kStopRight));
      std::string cmd;
      if (PeekCommand(&cmd) && cmd == "right") {
        SkipSpace();
        pos_ += 1 + cmd.size();
        SkipSpace();
        node->rightDelim = ReadDelimiter();
      } else {
        Fail("missing \\right");
      }
      return node;
    }

    // ---- manual delimiter sizing -------------------------------------------
    static const struct { const char* name; float factor; } kBigSizes[] = {
        {"big", 1.2f}, {"Big", 1.8f}, {"bigg", 2.4f}, {"Bigg", 3.0f},
        {"bigl", 1.2f}, {"Bigl", 1.8f}, {"biggl", 2.4f}, {"Biggl", 3.0f},
        {"bigr", 1.2f}, {"Bigr", 1.8f}, {"biggr", 2.4f}, {"Biggr", 3.0f},
    };
    for (const auto& entry : kBigSizes) {
      if (name == entry.name) {
        SkipSpace();
        const int cp = ReadDelimiter();
        auto node = std::make_unique<Node>();
        node->kind = NodeKind::Symbol;
        node->codepoint = cp;
        node->face = Face::MainRegular;
        node->atom = (name.back() == 'r') ? Atom::Close : Atom::Open;
        node->delimFixedSize = entry.factor;
        return node;
      }
    }

    // ---- environments ------------------------------------------------------
    if (name == "begin") return ParseEnvironment();

    // ---- big operators -----------------------------------------------------
    if (const BigOpDef* op = FindBigOp(name)) {
      auto node = std::make_unique<Node>();
      node->kind = NodeKind::BigOp;
      node->atom = Atom::Op;
      node->codepoint = op->codepoint;
      node->limits = op->limitsByDefault;
      std::string cmd;
      if (PeekCommand(&cmd) && (cmd == "limits" || cmd == "nolimits")) {
        SkipSpace();
        pos_ += 1 + cmd.size();
        node->limits = (cmd == "limits");
      }
      return node;
    }

    // ---- upright function names --------------------------------------------
    bool limits = false;
    if (IsFunctionName(name, &limits)) {
      NodePtr row = TextRow(name, Atom::Op);
      row->limits = limits;
      return row;
    }

    // ---- plain symbols -----------------------------------------------------
    if (const SymbolDef* sym = FindSymbol(name)) return MakeSymbol(*sym);

    Fail("unknown command: \\" + name);
    return TextRow(name, Atom::Ord);
  }

  int ReadDelimiter() {
    SkipSpace();
    if (AtEnd()) { Fail("missing delimiter"); return 0; }
    std::string token;
    if (PeekRaw() == '\\') {
      ++pos_;
      token = ReadCommandName();
    } else {
      const std::size_t start = pos_;
      NextCodepoint();
      token = std::string(src_.substr(start, pos_ - start));
    }
    const int cp = DelimiterCodepoint(token);
    if (cp < 0) {
      Fail("not a delimiter: " + token);
      return 0;
    }
    return cp;
  }

  // Parses one nucleus, with no scripts attached yet.
  NodePtr ParseUnit(int stopFlags = 0) {
    SkipSpace();
    if (AtEnd()) return MakeRow();

    const char c = PeekRaw();
    if (c == '{') {
      ++pos_;
      NodePtr body = ParseRow(kStopBrace);
      if (PeekRaw() == '}') ++pos_;
      else Fail("missing '}'");
      body->atom = Atom::Ord;
      return body;
    }
    if (c == '\\') {
      ++pos_;
      return ParseCommand(stopFlags);
    }
    const int cp = NextCodepoint();
    return MakeSymbol(ClassifyCharacter(cp));
  }

  // Parses a horizontal list, stopping (without consuming) at whatever the
  // caller asked to stop on.
  NodePtr ParseRow(int stopFlags, bool stopAtBracket = false) {
    NodePtr row = MakeRow();
    while (true) {
      SkipSpace();
      if (AtEnd()) break;

      const char c = PeekRaw();
      if (c == '}') {
        if (stopFlags & kStopBrace) break;
        Fail("unexpected '}'");
        break;
      }
      if (stopAtBracket && c == ']') break;
      if ((stopFlags & kStopCell) && c == '&') break;
      if (c == '\\') {
        std::string cmd;
        if (PeekCommand(&cmd)) {
          if ((stopFlags & kStopRight) && cmd == "right") break;
          if ((stopFlags & kStopCell) && (cmd == "\\" || cmd == "end")) break;
        }
      }

      NodePtr unit = ParseUnit(stopFlags);

      // A style switch swallowed the remainder of the list already.
      const bool consumedRest = (unit->kind == NodeKind::Styled);

      unit = AttachScripts(std::move(unit));
      row->kids.push_back(std::move(unit));
      if (consumedRest) break;
    }
    return row;
  }

  // Collects ^, _ and ' onto the nucleus just parsed.
  NodePtr AttachScripts(NodePtr nucleus) {
    NodePtr sup, sub;
    int primes = 0;

    while (true) {
      SkipSpace();
      const char c = PeekRaw();
      if (c == '^') {
        ++pos_;
        NodePtr value = ParseArgument();
        if (sup) { Fail("double superscript"); }
        else sup = std::move(value);
      } else if (c == '_') {
        ++pos_;
        NodePtr value = ParseArgument();
        if (sub) { Fail("double subscript"); }
        else sub = std::move(value);
      } else if (c == '\'') {
        ++pos_;
        ++primes;
      } else {
        break;
      }
    }

    if (primes > 0) {
      NodePtr row = MakeRow();
      for (int i = 0; i < primes; ++i) {
        row->kids.push_back(MakeSymbol({0x2032, Face::MainRegular, Atom::Ord}));
      }
      if (sup) {
        // Primes come first, then whatever was written as an explicit exponent.
        for (auto& kid : sup->kids) row->kids.push_back(std::move(kid));
        if (sup->kind != NodeKind::Row) row->kids.push_back(std::move(sup));
      }
      sup = std::move(row);
    }

    if (!sup && !sub) return nucleus;

    // Big operators carry their limits themselves so the layout pass can decide
    // between limits-above and script placement.
    if (nucleus->kind == NodeKind::BigOp ||
        (nucleus->kind == NodeKind::Row && nucleus->atom == Atom::Op && nucleus->limits)) {
      nucleus->sup = std::move(sup);
      nucleus->sub = std::move(sub);
      return nucleus;
    }

    auto node = std::make_unique<Node>();
    node->kind = NodeKind::Scripts;
    node->atom = nucleus->atom;
    node->kids.push_back(std::move(nucleus));
    node->sup = std::move(sup);
    node->sub = std::move(sub);
    return node;
  }

  std::string_view src_;
  std::size_t      pos_ = 0;
  std::string      error_;
};

}  // namespace

Parsed Parse(std::string_view source) { return Parser(source).Run(); }

}  // namespace sb::tex
