// Layout and drawing.
//
// This follows TeX's mlist-to-hlist conversion: every node becomes a box with a
// width, a height above the baseline and a depth below it, boxes are stacked
// with the shifts prescribed by the Computer Modern font parameters, and
// horizontal lists get TeX's inter-atom spacing. Coordinates inside a box are
// relative to its left edge and baseline, with y growing downwards to match
// raylib.
#include <algorithm>
#include <cmath>
#include <vector>

#include "tex/tex.h"

namespace sb::tex {
namespace {

using namespace texparams;

// ---------------------------------------------------------------------- boxes
struct Box {
  float             width = 0.0f;
  float             height = 0.0f;  // above the baseline
  float             depth = 0.0f;   // below the baseline
  Atom              atom = Atom::Ord;
  bool              glue = false;   // explicit spacing: not an atom
  bool              isSymbol = false;
  std::vector<Item> items;
};

void Offset(Item& it, float dx, float dy) {
  it.pen.x += dx;   it.pen.y += dy;
  it.rect.x += dx;  it.rect.y += dy;
  it.p0.x += dx;    it.p0.y += dy;
  it.p1.x += dx;    it.p1.y += dy;
}

void Merge(Box* dst, const Box& src, float dx, float dy) {
  dst->items.reserve(dst->items.size() + src.items.size());
  for (Item it : src.items) {
    Offset(it, dx, dy);
    dst->items.push_back(it);
  }
}

Item MakeRect(float x, float y, float w, float h) {
  Item it;
  it.kind = Item::Kind::Rect;
  it.rect = {x, y, w, h};
  return it;
}

// ---------------------------------------------------------------------- style
struct Ctx {
  FontStore* fonts = nullptr;
  float      baseEm = 16.0f;
  Style      style = Style::Text;
  bool       cramped = false;

  float em() const {
    switch (style) {
      case Style::Script: return baseEm * 0.7f;
      case Style::ScriptScript: return baseEm * 0.5f;
      default: return baseEm;
    }
  }
  bool display() const { return style == Style::Display; }
  float axis() const { return kAxisHeight * em(); }
  float rule() const { return kDefaultRuleThickness * em(); }
  float xheight() const { return kXHeight * em(); }
};

Style ScriptOf(Style s) {
  return (s == Style::Display || s == Style::Text) ? Style::Script : Style::ScriptScript;
}

Style FractionOf(Style s) {
  switch (s) {
    case Style::Display: return Style::Text;
    case Style::Text: return Style::Script;
    default: return Style::ScriptScript;
  }
}

Ctx With(Ctx cx, Style s, bool cramped) {
  cx.style = s;
  cx.cramped = cramped;
  return cx;
}

// ------------------------------------------------------------- atom spacing
// TeX's inter-atom spacing table (The TeXbook, chapter 18). Values are 0 = none,
// 1 = thin (3mu), 2 = medium (4mu), 3 = thick (5mu); the kScriptSuppressed flag
// marks the spaces that vanish in script and scriptscript styles.
constexpr unsigned char kScriptSuppressed = 0x10;
constexpr unsigned char kS = kScriptSuppressed;  // "text and display styles only"

constexpr unsigned char kSpacing[8][8] = {
    /* left \ right    Ord       Op        Bin       Rel       Open      Close  Punct     Inner */
    /* Ord   */ {0u,       1u,       2u | kS,  3u | kS,  0u,       0u,    0u,       1u | kS},
    /* Op    */ {1u,       1u,       0u,       3u | kS,  0u,       0u,    0u,       1u | kS},
    /* Bin   */ {2u | kS,  2u | kS,  0u,       0u,       2u | kS,  0u,    0u,       2u | kS},
    /* Rel   */ {3u | kS,  3u | kS,  0u,       0u,       3u | kS,  0u,    0u,       3u | kS},
    /* Open  */ {0u,       0u,       0u,       0u,       0u,       0u,    0u,       0u},
    /* Close */ {0u,       1u,       2u | kS,  3u | kS,  0u,       0u,    0u,       1u | kS},
    /* Punct */ {1u | kS,  1u | kS,  0u,       1u | kS,  1u | kS,  1u|kS, 1u | kS,  1u | kS},
    /* Inner */ {1u | kS,  1u,       2u | kS,  3u | kS,  1u | kS,  0u,    1u | kS,  1u | kS},
};

float AtomSpace(Atom left, Atom right, const Ctx& cx) {
  const unsigned char entry = kSpacing[static_cast<int>(left)][static_cast<int>(right)];
  const unsigned char amount = entry & 0x0Fu;
  if (amount == 0) return 0.0f;
  const bool suppressed = (entry & kScriptSuppressed) != 0;
  if (suppressed && (cx.style == Style::Script || cx.style == Style::ScriptScript)) return 0.0f;
  // 1 mu is 1/18 em.
  const float mu = cx.em() / 18.0f;
  return static_cast<float>(amount + 2) * mu;  // 3mu, 4mu, 5mu
}

// ---------------------------------------------------------------- primitives
Box LayoutNode(const Node& node, Ctx cx);
Box LayoutNodeInner(const Node& node, Ctx cx);

Box GlyphBox(int codepoint, Face face, Atom atom, Ctx cx, float emOverride = 0.0f) {
  const float em = (emOverride > 0.0f) ? emOverride : cx.em();
  Box box;
  box.atom = atom;
  box.isSymbol = true;

  Glyph g = cx.fonts->GetGlyph(face, codepoint, em);
  if (!g.present) {
    // Fall back through the faces most likely to carry a stray codepoint rather
    // than dropping it silently.
    for (Face alt : {Face::MainRegular, Face::AMS, Face::MathItalic, Face::Size1}) {
      if (alt == face) continue;
      g = cx.fonts->GetGlyph(alt, codepoint, em);
      if (g.present) { face = alt; break; }
    }
  }
  if (!g.present) return box;

  box.width = g.advance;
  box.height = g.top;
  box.depth = std::max(0.0f, g.height - g.top);

  Item it;
  it.kind = Item::Kind::Glyph;
  it.face = face;
  it.codepoint = codepoint;
  it.emPx = em;
  it.pen = {0.0f, 0.0f};
  box.items.push_back(it);
  return box;
}

// Picks the smallest size variant of a delimiter that reaches `needed` total
// height, then centres it on the math axis the way TeX does.
Box DelimiterBox(int codepoint, float needed, Atom atom, Ctx cx) {
  Box box;
  box.atom = atom;
  if (codepoint == 0) return box;  // LaTeX's "." -- an empty fence

  static const Face kVariants[] = {Face::MainRegular, Face::Size1, Face::Size2,
                                   Face::Size3, Face::Size4};
  const float em = cx.em();

  Face chosen = Face::MainRegular;
  Glyph best{};
  for (Face face : kVariants) {
    if (!cx.fonts->HasGlyph(face, codepoint)) continue;
    const Glyph g = cx.fonts->GetGlyph(face, codepoint, em);
    if (!g.present) continue;
    chosen = face;
    best = g;
    if (g.height >= needed) break;  // tall enough; stop growing
  }
  if (!best.present) return box;

  const float glyphHeight = best.top;
  const float glyphDepth = std::max(0.0f, best.height - best.top);

  // Shift so the glyph's vertical centre lands on the axis.
  const float centre = (glyphHeight - glyphDepth) * 0.5f;
  const float shift = centre - cx.axis();

  box.width = best.advance;
  box.height = glyphHeight - shift;
  box.depth = glyphDepth + shift;

  Item it;
  it.kind = Item::Kind::Glyph;
  it.face = chosen;
  it.codepoint = codepoint;
  it.emPx = em;
  it.pen = {0.0f, shift};
  box.items.push_back(it);
  return box;
}

// --------------------------------------------------------------- horizontal
Box LayoutRow(const std::vector<NodePtr>& kids, Ctx cx, Atom rowAtom) {
  std::vector<Box> boxes;
  boxes.reserve(kids.size());
  for (const NodePtr& kid : kids) boxes.push_back(LayoutNode(*kid, cx));

  // TeX reclassifies a Bin atom as Ord when it cannot actually be binary --
  // leading minus signs, or one operator straight after another. Without this
  // pass "-x" gets the wide spacing of a subtraction.
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    if (boxes[i].glue) continue;
    if (boxes[i].atom != Atom::Bin) continue;

    // Find the previous non-glue atom.
    std::size_t prev = i;
    bool havePrev = false;
    for (std::size_t j = i; j-- > 0;) {
      if (boxes[j].glue) continue;
      prev = j;
      havePrev = true;
      break;
    }
    if (!havePrev) {
      boxes[i].atom = Atom::Ord;
      continue;
    }
    switch (boxes[prev].atom) {
      case Atom::Bin: case Atom::Op: case Atom::Rel:
      case Atom::Open: case Atom::Punct:
        boxes[i].atom = Atom::Ord;
        break;
      default:
        break;
    }
  }
  // ... and a Bin that turns out to be followed by a relation or a closer is
  // not binary either.
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    if (boxes[i].glue || boxes[i].atom == Atom::Bin) continue;
    if (boxes[i].atom != Atom::Rel && boxes[i].atom != Atom::Close &&
        boxes[i].atom != Atom::Punct) {
      continue;
    }
    for (std::size_t j = i; j-- > 0;) {
      if (boxes[j].glue) continue;
      if (boxes[j].atom == Atom::Bin) boxes[j].atom = Atom::Ord;
      break;
    }
  }

  Box row;
  row.atom = rowAtom;
  float x = 0.0f;
  bool  havePrevAtom = false;
  Atom  prevAtom = Atom::Ord;

  for (const Box& b : boxes) {
    if (!b.glue && havePrevAtom) x += AtomSpace(prevAtom, b.atom, cx);
    Merge(&row, b, x, 0.0f);
    row.height = std::max(row.height, b.height);
    row.depth = std::max(row.depth, b.depth);
    x += b.width;
    if (!b.glue) { prevAtom = b.atom; havePrevAtom = true; }
  }
  row.width = x;
  if (boxes.size() == 1) row.isSymbol = boxes[0].isSymbol;
  return row;
}

// ------------------------------------------------------------------ scripts
Box LayoutScriptsOn(Box base, const Node* supNode, const Node* subNode, Ctx cx) {
  const float em = cx.em();
  const Ctx scriptCx = With(cx, ScriptOf(cx.style), cx.cramped);
  const float scriptEm = scriptCx.em();

  Box sup, sub;
  const bool hasSup = (supNode != nullptr);
  const bool hasSub = (subNode != nullptr);
  if (hasSup) sup = LayoutNode(*supNode, With(cx, ScriptOf(cx.style), cx.cramped));
  if (hasSub) sub = LayoutNode(*subNode, With(cx, ScriptOf(cx.style), true));

  // A bare character needs no drop; a composite box hangs its scripts off its
  // own extents.
  float supShift = base.isSymbol ? 0.0f : (base.height - kSupDrop * scriptEm);
  float subShift = base.isSymbol ? 0.0f : (base.depth + kSubDrop * scriptEm);

  Box out;
  out.atom = base.atom;
  const float scriptSpace = 0.05f * em;

  if (hasSup && !hasSub) {
    const float p = cx.display() ? kSup1 : (cx.cramped ? kSup3 : kSup2);
    supShift = std::max({supShift, p * em, sup.depth + 0.25f * cx.xheight()});
  } else if (hasSub && !hasSup) {
    subShift = std::max({subShift, kSub1 * em, sub.height - 0.8f * cx.xheight()});
  } else if (hasSup && hasSub) {
    const float p = cx.display() ? kSup1 : (cx.cramped ? kSup3 : kSup2);
    supShift = std::max({supShift, p * em, sup.depth + 0.25f * cx.xheight()});
    subShift = std::max(subShift, kSub2 * em);

    // Keep at least four rule thicknesses of clear air between the two.
    const float theta = cx.rule();
    const float gap = (supShift - sup.depth) - (sub.height - subShift);
    if (gap < 4.0f * theta) {
      subShift = 4.0f * theta - supShift + sup.depth + sub.height;
      const float psi = 0.8f * cx.xheight() - (supShift - sup.depth);
      if (psi > 0.0f) {
        supShift += psi;
        subShift -= psi;
      }
    }
  }

  Merge(&out, base, 0.0f, 0.0f);
  out.height = base.height;
  out.depth = base.depth;

  const float scriptX = base.width;
  if (hasSup) {
    Merge(&out, sup, scriptX, -supShift);
    out.height = std::max(out.height, supShift + sup.height);
    out.depth = std::max(out.depth, sup.depth - supShift);
  }
  if (hasSub) {
    Merge(&out, sub, scriptX, subShift);
    out.depth = std::max(out.depth, subShift + sub.depth);
    out.height = std::max(out.height, sub.height - subShift);
  }

  float scriptWidth = 0.0f;
  if (hasSup) scriptWidth = std::max(scriptWidth, sup.width);
  if (hasSub) scriptWidth = std::max(scriptWidth, sub.width);
  out.width = base.width + scriptWidth + (scriptWidth > 0.0f ? scriptSpace : 0.0f);
  return out;
}

// Limits stacked above and below, as display-style big operators want.
Box LayoutLimitsOn(const Box& op, const Node* supNode, const Node* subNode, Ctx cx) {
  const float em = cx.em();
  Box sup, sub;
  if (supNode) sup = LayoutNode(*supNode, With(cx, ScriptOf(cx.style), cx.cramped));
  if (subNode) sub = LayoutNode(*subNode, With(cx, ScriptOf(cx.style), true));

  const float width = std::max({op.width, sup.width, sub.width});
  Box out;
  out.atom = Atom::Op;
  out.width = width;

  const float opX = (width - op.width) * 0.5f;
  Merge(&out, op, opX, 0.0f);
  out.height = op.height;
  out.depth = op.depth;

  if (supNode) {
    const float kern = std::max(kBigOpSpacing1 * em, kBigOpSpacing3 * em - sup.depth);
    const float baselineY = -(op.height + kern + sup.depth);
    Merge(&out, sup, (width - sup.width) * 0.5f, baselineY);
    out.height = std::max(out.height, -baselineY + sup.height + kBigOpSpacing5 * em);
  }
  if (subNode) {
    const float kern = std::max(kBigOpSpacing2 * em, kBigOpSpacing4 * em - sub.height);
    const float baselineY = op.depth + kern + sub.height;
    Merge(&out, sub, (width - sub.width) * 0.5f, baselineY);
    out.depth = std::max(out.depth, baselineY + sub.depth + kBigOpSpacing5 * em);
  }
  return out;
}

// ---------------------------------------------------------------- fractions
Box LayoutFraction(const Node& node, Ctx cx) {
  const Style fracStyle = node.styleIsExplicit ? node.style : cx.style;
  Ctx inner = With(cx, fracStyle, cx.cramped);
  const float em = inner.em();
  const bool display = (fracStyle == Style::Display);

  const Box num = LayoutNode(*node.kids[0], With(inner, FractionOf(fracStyle), inner.cramped));
  const Box den = LayoutNode(*node.kids[1], With(inner, FractionOf(fracStyle), true));

  const float theta = kDefaultRuleThickness * em;
  const float axis = kAxisHeight * em;

  float shiftUp = display ? kNum1 * em : (node.hasRule ? kNum2 * em : kNum3 * em);
  float shiftDown = display ? kDenom1 * em : kDenom2 * em;

  if (node.hasRule) {
    const float phi = display ? 3.0f * theta : theta;
    const float clearUp = (shiftUp - num.depth) - (axis + theta * 0.5f);
    if (clearUp < phi) shiftUp += phi - clearUp;
    const float clearDown = (axis - theta * 0.5f) - (den.height - shiftDown);
    if (clearDown < phi) shiftDown += phi - clearDown;
  } else {
    const float phi = display ? 7.0f * theta : 3.0f * theta;
    const float clearance = (shiftUp - num.depth) - (den.height - shiftDown);
    if (clearance < phi) {
      const float extra = (phi - clearance) * 0.5f;
      shiftUp += extra;
      shiftDown += extra;
    }
  }

  Box body;
  body.atom = Atom::Inner;
  const float width = std::max(num.width, den.width);
  body.width = width;

  Merge(&body, num, (width - num.width) * 0.5f, -shiftUp);
  Merge(&body, den, (width - den.width) * 0.5f, shiftDown);
  body.height = shiftUp + num.height;
  body.depth = shiftDown + den.depth;

  if (node.hasRule) {
    body.items.push_back(MakeRect(0.0f, -axis - theta * 0.5f, width, theta));
    body.height = std::max(body.height, axis + theta * 0.5f);
    body.depth = std::max(body.depth, theta * 0.5f - axis);
  }

  if (node.leftDelim == 0 && node.rightDelim == 0) return body;

  // \binom wraps the stack in parentheses grown to fit.
  const float needed = 2.0f * std::max(body.height - axis, body.depth + axis);
  const Box left = DelimiterBox(node.leftDelim, needed, Atom::Open, inner);
  const Box right = DelimiterBox(node.rightDelim, needed, Atom::Close, inner);

  Box out;
  out.atom = Atom::Inner;
  Merge(&out, left, 0.0f, 0.0f);
  Merge(&out, body, left.width, 0.0f);
  Merge(&out, right, left.width + body.width, 0.0f);
  out.width = left.width + body.width + right.width;
  out.height = std::max({left.height, body.height, right.height});
  out.depth = std::max({left.depth, body.depth, right.depth});
  return out;
}

// ------------------------------------------------------------------ radicals
Box LayoutRadical(const Node& node, Ctx cx) {
  const float em = cx.em();
  const Box inner = LayoutNode(*node.kids[0], With(cx, cx.style, true));

  const float theta = cx.rule();
  const float phi = cx.display() ? cx.xheight() : theta;
  const float gap = theta + phi * 0.25f;

  const float innerTotal = inner.height + inner.depth;
  const float needed = innerTotal + gap + theta;

  // The surd is a glyph like any other delimiter, so reuse the growth ladder.
  static const Face kVariants[] = {Face::MainRegular, Face::Size1, Face::Size2,
                                   Face::Size3, Face::Size4};
  Face  surdFace = Face::MainRegular;
  Glyph surd{};
  for (Face face : kVariants) {
    if (!cx.fonts->HasGlyph(face, 0x221A)) continue;
    const Glyph g = cx.fonts->GetGlyph(face, 0x221A, em);
    if (!g.present) continue;
    surdFace = face;
    surd = g;
    if (g.height >= needed) break;
  }

  Box out;
  out.atom = Atom::Ord;
  if (!surd.present) return inner;

  // Bar sits `gap` above the radicand, with the surd hanging from it.
  const float barTop = inner.height + gap;
  const float surdInk = surd.height;
  const float surdWidth = surd.advance;

  // Draw the surd with its ink top at -barTop - theta.
  const float glyphTopY = -(barTop + theta);
  const float penY = glyphTopY + surd.top;

  Item glyph;
  glyph.kind = Item::Kind::Glyph;
  glyph.face = surdFace;
  glyph.codepoint = 0x221A;
  glyph.emPx = em;
  glyph.pen = {0.0f, penY};
  out.items.push_back(glyph);

  // The overbar, running from the surd's tip across the radicand.
  out.items.push_back(MakeRect(surdWidth, -(barTop + theta), inner.width, theta));

  Merge(&out, inner, surdWidth, 0.0f);
  out.width = surdWidth + inner.width;
  out.height = barTop + theta;
  out.depth = std::max(inner.depth, surdInk - barTop - theta);

  if (node.index) {
    const Box idx = LayoutNode(*node.index, With(cx, Style::ScriptScript, cx.cramped));
    // TeX raises the index to about 60% of the radical's height.
    const float lift = 0.6f * (out.height + out.depth);
    Box shifted;
    Merge(&shifted, idx, 0.0f, -lift);

    Box combined;
    combined.atom = Atom::Ord;
    const float indent = std::max(0.0f, idx.width - 0.4f * surdWidth);
    Merge(&combined, shifted, 0.0f, 0.0f);
    Merge(&combined, out, indent, 0.0f);
    combined.width = indent + out.width;
    combined.height = std::max(out.height, lift + idx.height);
    combined.depth = std::max(out.depth, idx.depth - lift);
    return combined;
  }
  return out;
}

// ------------------------------------------------------------------- arrays
Box LayoutArray(const Node& node, Ctx cx) {
  const float em = cx.em();
  const Ctx cellCx = With(cx, cx.style == Style::Display ? Style::Text : cx.style, cx.cramped);

  std::vector<std::vector<Box>> grid;
  std::size_t columns = 0;
  for (const auto& row : node.rows) {
    grid.emplace_back();
    for (const NodePtr& cell : row) grid.back().push_back(LayoutNode(*cell, cellCx));
    columns = std::max(columns, grid.back().size());
  }

  std::vector<float> colWidth(columns, 0.0f);
  for (const auto& row : grid) {
    for (std::size_t c = 0; c < row.size(); ++c) colWidth[c] = std::max(colWidth[c], row[c].width);
  }

  const float colGap = node.arrayIsCases ? 1.0f * em : 0.9f * em;
  const float rowGap = 0.35f * em;

  // Stack the rows, then recentre the whole block on the axis.
  std::vector<float> baselineY(grid.size(), 0.0f);
  float firstHeight = 0.0f;
  float y = 0.0f;
  for (std::size_t r = 0; r < grid.size(); ++r) {
    float rowHeight = 0.0f, rowDepth = 0.0f;
    for (const Box& b : grid[r]) {
      rowHeight = std::max(rowHeight, b.height);
      rowDepth = std::max(rowDepth, b.depth);
    }
    if (r == 0) firstHeight = rowHeight;
    else y += rowGap + rowHeight;
    baselineY[r] = y;
    y += rowDepth;
  }
  // The block spans [-firstHeight, y]; slide its midpoint onto the axis.
  const float shift = (y - firstHeight) * 0.5f + cx.axis();

  Box body;
  body.atom = Atom::Inner;
  float totalWidth = 0.0f;
  for (std::size_t c = 0; c < columns; ++c) {
    totalWidth += colWidth[c];
    if (c + 1 < columns) totalWidth += colGap;
  }
  body.width = totalWidth;

  for (std::size_t r = 0; r < grid.size(); ++r) {
    float x = 0.0f;
    for (std::size_t c = 0; c < grid[r].size(); ++c) {
      // Matrices centre their columns; cases left-aligns, as LaTeX does.
      const float pad = node.arrayIsCases ? 0.0f : (colWidth[c] - grid[r][c].width) * 0.5f;
      Merge(&body, grid[r][c], x + pad, baselineY[r] - shift);
      x += colWidth[c] + colGap;
    }
  }
  for (std::size_t r = 0; r < grid.size(); ++r) {
    for (const Box& b : grid[r]) {
      body.height = std::max(body.height, -(baselineY[r] - shift) + b.height);
      body.depth = std::max(body.depth, (baselineY[r] - shift) + b.depth);
    }
  }

  if (node.arrayLeft == 0 && node.arrayRight == 0) return body;

  const float needed = 2.0f * std::max(body.height - cx.axis(), body.depth + cx.axis());
  const Box left = DelimiterBox(node.arrayLeft, needed, Atom::Open, cx);
  const Box right = DelimiterBox(node.arrayRight, needed, Atom::Close, cx);
  const float pad = 0.15f * em;

  Box out;
  out.atom = Atom::Inner;
  Merge(&out, left, 0.0f, 0.0f);
  Merge(&out, body, left.width + pad, 0.0f);
  Merge(&out, right, left.width + pad + body.width + pad, 0.0f);
  out.width = left.width + body.width + right.width + 2.0f * pad;
  out.height = std::max({left.height, body.height, right.height});
  out.depth = std::max({left.depth, body.depth, right.depth});
  return out;
}

// ------------------------------------------------------------------ dispatch
Box LayoutNodeInner(const Node& node, Ctx cx) {
  const float em = cx.em();

  switch (node.kind) {
    case NodeKind::Row: {
      Box row = LayoutRow(node.kids, cx, node.atom);
      if (node.sup || node.sub) {
        return node.limits && cx.display()
                   ? LayoutLimitsOn(row, node.sup.get(), node.sub.get(), cx)
                   : LayoutScriptsOn(std::move(row), node.sup.get(), node.sub.get(), cx);
      }
      return row;
    }

    case NodeKind::Symbol: {
      if (node.delimFixedSize > 0.0f) {
        return DelimiterBox(node.codepoint, node.delimFixedSize * em, node.atom, cx);
      }
      return GlyphBox(node.codepoint, node.face, node.atom, cx);
    }

    case NodeKind::Space: {
      Box box;
      box.glue = true;
      box.width = node.amount * em;
      return box;
    }

    case NodeKind::Styled: {
      Ctx inner = With(cx, node.style, cx.cramped);
      return LayoutRow(node.kids[0]->kids, inner, Atom::Ord);
    }

    case NodeKind::Scripts: {
      Box base = LayoutNode(*node.kids[0], cx);
      return LayoutScriptsOn(std::move(base), node.sup.get(), node.sub.get(), cx);
    }

    case NodeKind::Fraction:
      return LayoutFraction(node, cx);

    case NodeKind::Radical:
      return LayoutRadical(node, cx);

    case NodeKind::BigOp: {
      // Display style uses the taller Size2 cut of the operator.
      const Face face = cx.display() ? Face::Size2 : Face::Size1;
      Box op = GlyphBox(node.codepoint, face, Atom::Op, cx);
      // Big operators are centred on the axis rather than sitting on the
      // baseline.
      const float centre = (op.height - op.depth) * 0.5f;
      const float shift = centre - cx.axis();
      for (Item& it : op.items) Offset(it, 0.0f, shift);
      op.height -= shift;
      op.depth += shift;
      op.isSymbol = false;

      if (!node.sup && !node.sub) return op;
      if (node.limits && cx.display()) {
        return LayoutLimitsOn(op, node.sup.get(), node.sub.get(), cx);
      }
      return LayoutScriptsOn(std::move(op), node.sup.get(), node.sub.get(), cx);
    }

    case NodeKind::Delimited: {
      const Box body = LayoutNode(*node.kids[0], cx);
      const float needed = 2.0f * std::max(body.height - cx.axis(), body.depth + cx.axis());
      const Box left = DelimiterBox(node.leftDelim, needed, Atom::Open, cx);
      const Box right = DelimiterBox(node.rightDelim, needed, Atom::Close, cx);

      Box out;
      out.atom = Atom::Inner;
      Merge(&out, left, 0.0f, 0.0f);
      Merge(&out, body, left.width, 0.0f);
      Merge(&out, right, left.width + body.width, 0.0f);
      out.width = left.width + body.width + right.width;
      out.height = std::max({left.height, body.height, right.height});
      out.depth = std::max({left.depth, body.depth, right.depth});
      return out;
    }

    case NodeKind::Accent: {
      const Box base = LayoutNode(*node.kids[0], With(cx, cx.style, true));

      Box out;
      out.atom = base.atom;
      Merge(&out, base, 0.0f, 0.0f);
      out.width = base.width;
      out.height = base.height;
      out.depth = base.depth;

      // Work from the raw glyph: an accent sits entirely above the baseline, so
      // its Box depth is 0 and the ink extent cannot be recovered from the box.
      const Glyph g = cx.fonts->GetGlyph(node.face, node.codepoint, em);
      if (!g.present) return out;

      // Sit the accent just clear of the nucleus, but never lower than the
      // x-height -- short letters would otherwise wear it too low.
      const float inkBottom = std::max(base.height, cx.xheight()) + 0.03f * em;
      // Drawn at pen y = dy, the glyph ink runs from (dy - g.top) to
      // (dy - g.top + g.height); we want the lower edge at -inkBottom.
      const float dy = g.top - g.height - inkBottom;
      // Centre on the ink, not the advance: \vec is the combining arrow
      // U+20D7, whose advance is zero and whose ink sits at a negative
      // bearing, so centring by advance would push it off to the left.
      const float dx = base.width * 0.5f - g.bearingX - g.width * 0.5f;

      Item it;
      it.kind = Item::Kind::Glyph;
      it.face = node.face;
      it.codepoint = node.codepoint;
      it.emPx = em;
      it.pen = {dx, dy};
      out.items.push_back(it);

      out.height = std::max(out.height, inkBottom + g.height);
      return out;
    }

    case NodeKind::Overline: {
      const Box base = LayoutNode(*node.kids[0], With(cx, cx.style, true));
      const float theta = cx.rule();
      Box out;
      out.atom = Atom::Ord;
      Merge(&out, base, 0.0f, 0.0f);
      out.width = base.width;
      out.depth = base.depth;
      const float barBottom = base.height + 3.0f * theta;
      out.items.push_back(MakeRect(0.0f, -(barBottom + theta), base.width, theta));
      out.height = barBottom + theta + theta;
      return out;
    }

    case NodeKind::Underline: {
      const Box base = LayoutNode(*node.kids[0], cx);
      const float theta = cx.rule();
      Box out;
      out.atom = Atom::Ord;
      Merge(&out, base, 0.0f, 0.0f);
      out.width = base.width;
      out.height = base.height;
      const float barTop = base.depth + 3.0f * theta;
      out.items.push_back(MakeRect(0.0f, barTop, base.width, theta));
      out.depth = barTop + theta + theta;
      return out;
    }

    case NodeKind::Negated: {
      Box base = LayoutNode(*node.kids[0], cx);
      const float cx0 = base.width * 0.5f;
      const float axis = cx.axis();
      Item line;
      line.kind = Item::Kind::Line;
      line.p0 = {cx0 - 0.16f * em, -axis + 0.34f * em};
      line.p1 = {cx0 + 0.16f * em, -axis - 0.34f * em};
      line.thickness = std::max(1.0f, cx.rule());
      base.items.push_back(line);
      base.height = std::max(base.height, axis + 0.34f * em);
      base.depth = std::max(base.depth, 0.34f * em - axis);
      base.isSymbol = false;
      return base;
    }

    case NodeKind::Array:
      return LayoutArray(node, cx);
  }
  return Box{};
}

// A marked subtree stamps its tag on everything it produced, so that whoever
// asked for the mark can find those items again. The innermost mark wins:
// anything already tagged came from a marked subtree of this one.
Box LayoutNode(const Node& node, Ctx cx) {
  Box box = LayoutNodeInner(node, cx);
  if (node.mark != 0) {
    for (Item& it : box.items) {
      if (it.mark == 0) it.mark = node.mark;
    }
  }
  return box;
}

}  // namespace

Layout BuildLayout(const Parsed& parsed, FontStore& fonts, float emPx, bool display) {
  Layout out;
  out.error = parsed.error;
  if (!parsed.root) return out;

  Ctx cx;
  cx.fonts = &fonts;
  cx.baseEm = emPx;
  cx.style = display ? Style::Display : Style::Text;
  cx.cramped = false;

  const Box box = LayoutNode(*parsed.root, cx);
  out.width = box.width;
  out.height = box.height;
  out.depth = box.depth;
  out.items = box.items;
  return out;
}

Layout Render(std::string_view source, FontStore& fonts, float emPx, bool display) {
  return BuildLayout(Parse(source), fonts, emPx, display);
}

void DrawLayout(const Layout& layout, FontStore& fonts, Vector2 origin, Color tint) {
  for (const Item& it : layout.items) {
    switch (it.kind) {
      case Item::Kind::Glyph: {
        const Glyph g = fonts.GetGlyph(it.face, it.codepoint, it.emPx);
        if (!g.present || g.atlas == nullptr || g.width <= 0.0f) break;
        const Rectangle dst = {origin.x + it.pen.x + g.bearingX,
                               origin.y + it.pen.y - g.top, g.width, g.height};
        DrawTexturePro(*g.atlas, g.src, dst, {0.0f, 0.0f}, 0.0f, tint);
        break;
      }
      case Item::Kind::Rect: {
        const Rectangle r = {origin.x + it.rect.x, origin.y + it.rect.y,
                             std::max(it.rect.width, 0.0f), std::max(it.rect.height, 0.75f)};
        DrawRectangleRec(r, tint);
        break;
      }
      case Item::Kind::Line: {
        DrawLineEx({origin.x + it.p0.x, origin.y + it.p0.y},
                   {origin.x + it.p1.x, origin.y + it.p1.y}, it.thickness, tint);
        break;
      }
    }
  }
}

}  // namespace sb::tex
