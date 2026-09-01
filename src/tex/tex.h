// A TeX math typesetter.
//
// This is a real implementation of TeX's box-and-glue model for math mode, not
// a string-substitution approximation: atoms are classified (Ord/Op/Bin/Rel/
// Open/Close/Punct/Inner), inter-atom spacing comes from TeX's spacing table,
// and fractions, radicals, scripts and delimiters are positioned with the
// Computer Modern font parameters in fontstore.h. That is what makes the output
// read as LaTeX rather than as "maths-ish text".
//
// Supported: the symbol set in tex_symbols.cpp (Greek, operators, relations,
// arrows, AMS extras), super/subscripts with proper limit placement on big
// operators, \frac \dfrac \tfrac \binom, \sqrt with an optional index,
// \left...\right with auto-grown delimiters, \big..\Bigg, accents, \overline,
// \underline, \text, \operatorname, the \math* font switches, explicit spacing,
// style switches, and the matrix/cases environments.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fontstore.h"
#include "raylib.h"

namespace sb::tex {

// TeX's eight atom classes. The pair (left atom, right atom) selects the space
// TeX inserts between them, which is most of why real maths typesetting looks
// the way it does.
enum class Atom : unsigned char { Ord, Op, Bin, Rel, Open, Close, Punct, Inner };

// Math styles, in TeX's order. Display and Text share a size; Script and
// ScriptScript shrink.
enum class Style : unsigned char { Display, Text, Script, ScriptScript };

enum class NodeKind : unsigned char {
  Row,        // a horizontal list
  Symbol,     // one glyph
  Scripts,    // base with optional superscript/subscript
  Fraction,   // numerator over denominator, with or without a rule
  Radical,    // \sqrt, optional index
  BigOp,      // \sum, \int, ... (limit placement depends on style)
  Delimited,  // \left ... \right
  Accent,     // \hat, \bar, \vec, ...
  Overline,
  Underline,
  Space,      // explicit \, \; \quad ...
  Styled,     // \displaystyle and friends
  Negated,    // \not applied to the child
  Array,      // matrix / cases environments
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
  NodeKind kind = NodeKind::Row;
  Atom     atom = Atom::Ord;

  // Set by \sbref{n}{...}: an app-private tag that says "this subtree is the
  // n-th thing the generator wanted to be able to find again". It survives into
  // every Item the subtree lays out, so a caller can ask where on screen a
  // particular piece of the formula ended up. Nothing about typesetting.
  int mark = 0;

  // Symbol / BigOp / Accent
  int  codepoint = 0;
  Face face = Face::MainRegular;

  // BigOp: true when limits belong above and below rather than as scripts.
  bool limits = false;
  // Fraction: false for \binom, which stacks without a rule.
  bool hasRule = true;

  // Delimited: 0 means "no delimiter on this side" (LaTeX's `.`).
  int   leftDelim = 0;
  int   rightDelim = 0;
  // Fixed delimiter growth for \big..\Bigg; 0 means size to the contents.
  float delimFixedSize = 0.0f;

  float amount = 0.0f;          // Space: width in em
  Style style = Style::Text;    // Styled: the style to switch into
  // Fraction: \dfrac and \tfrac pin the style, plain \frac inherits it.
  bool  styleIsExplicit = false;

  std::vector<NodePtr> kids;    // Row items, or the operands of the node
  NodePtr sup;                  // Scripts / BigOp
  NodePtr sub;                  // Scripts / BigOp
  NodePtr index;                // Radical

  // Array: rows of cells, plus the fences to wrap them in.
  std::vector<std::vector<NodePtr>> rows;
  int  arrayLeft = 0;
  int  arrayRight = 0;
  bool arrayIsCases = false;
};

struct Parsed {
  NodePtr     root;
  std::string error;  // non-empty if the source could not be parsed
};

// Parses a LaTeX math-mode fragment. Never throws; on a syntax error `error` is
// set and `root` holds whatever was understood up to that point.
Parsed Parse(std::string_view source);

// ---------------------------------------------------------------------------
// Laid-out output: a flat list of things to draw, in coordinates relative to
// the left edge of the formula and its baseline (y grows downwards).

struct Item {
  enum class Kind : unsigned char { Glyph, Rect, Line };
  Kind kind = Kind::Glyph;

  // Glyph
  Face    face = Face::MainRegular;
  int     codepoint = 0;
  float   emPx = 0.0f;
  Vector2 pen{};  // x = pen position, y = baseline

  // Rect (fraction bars, overlines, ...)
  Rectangle rect{};

  // Line (radical strokes, the slash of \not)
  Vector2 p0{}, p1{};
  float   thickness = 0.0f;

  // The \sbref tag of the innermost marked subtree this came from, or 0. It is
  // how a caller finds where a particular piece of the formula was drawn.
  int mark = 0;
};

struct Layout {
  float             width = 0.0f;
  float             height = 0.0f;  // above the baseline
  float             depth = 0.0f;   // below the baseline
  std::vector<Item> items;
  std::string       error;
};

// Lays the parsed formula out at `emPx`. `display` selects display style
// (bigger operators, limits above and below) over inline text style.
Layout BuildLayout(const Parsed& parsed, FontStore& fonts, float emPx, bool display);

// Convenience: parse and lay out in one call.
Layout Render(std::string_view source, FontStore& fonts, float emPx, bool display);

// Draws a laid-out formula with its left edge and baseline at `origin`.
void DrawLayout(const Layout& layout, FontStore& fonts, Vector2 origin, Color tint);

// ---------------------------------------------------------------------------
// Symbol tables (defined in tex_symbols.cpp).

struct SymbolDef {
  int  codepoint;
  Face face;
  Atom atom;
};

struct BigOpDef {
  int  codepoint;   // as found in the Size1 face
  bool limitsByDefault;
};

// `name` excludes the leading backslash. Returns nullptr when unknown.
const SymbolDef* FindSymbol(std::string_view name);
const BigOpDef*  FindBigOp(std::string_view name);
const SymbolDef* FindAccent(std::string_view name);
// Upright multi-letter operators: \sin, \log, \lim, ...
bool             IsFunctionName(std::string_view name, bool* takesLimits);
// Maps a delimiter command or literal character to a codepoint; 0 if not one.
int              DelimiterCodepoint(std::string_view token);

// Classification for a bare character in math mode.
SymbolDef ClassifyCharacter(int codepoint);

}  // namespace sb::tex
