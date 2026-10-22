#include "transition.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "blocks.h"
#include "mathline.h"
#include "rlgl.h"
#include "text.h"
#include "theme.h"

namespace sb::transition {
namespace {

morph::Contour Placed(const std::vector<Vector2>& contour, Vector2 origin) {
  morph::Contour out;
  out.reserve(contour.size());
  for (const Vector2& p : contour) out.push_back({origin.x + p.x, origin.y + p.y});
  return out;
}

void AddGlyph(FontStore& fonts, Face face, int codepoint, float emPx, Vector2 pen,
              morph::Piece* piece) {
  for (const std::vector<Vector2>& contour : fonts.GlyphContours(face, codepoint)) {
    morph::Contour& c = piece->contours.emplace_back();
    for (const Vector2& p : contour) c.push_back({pen.x + p.x * emPx, pen.y - p.y * emPx});
  }
}

// What a character of code reads as, where the formula writes it differently.
std::string GlyphKey(int codepoint) {
  switch (codepoint) {
    case '*': codepoint = 0x22C5; break;   // \cdot
    case '-': codepoint = 0x2212; break;   // the minus sign
    case '/': return "rule";
    default: break;
  }
  return "g:" + std::to_string(codepoint);
}

bool IsLetter(int c) { return c < 128 && (std::isalpha(c) != 0 || c == '_'); }

}  // namespace

std::vector<morph::Piece> CodePieces(FontStore& fonts, const std::string& code, float emPx,
                                     Vector2 pen) {
  std::vector<morph::Piece> out;
  std::size_t i = 0;
  while (i < code.size()) {
    const unsigned char c = static_cast<unsigned char>(code[i]);
    if (IsLetter(c)) {
      std::size_t end = i;
      while (end < code.size() && (IsLetter(static_cast<unsigned char>(code[end])) ||
                                   std::isdigit(static_cast<unsigned char>(code[end])) != 0)) {
        ++end;
      }
      morph::Piece& piece = out.emplace_back();
      piece.key = "id:" + code.substr(i, end - i);
      for (; i < end; ++i) {
        AddGlyph(fonts, Face::Mono, code[i], emPx, pen, &piece);
        pen.x += fonts.Advance(Face::Mono, code[i], emPx);
      }
      continue;
    }
    const int cp = text::NextCodepoint(code, &i);
    if (cp != ' ' && cp != '\t') {
      morph::Piece& piece = out.emplace_back();
      piece.key = GlyphKey(cp);
      AddGlyph(fonts, Face::Mono, cp, emPx, pen, &piece);
    }
    pen.x += fonts.Advance(Face::Mono, cp, emPx);
  }
  return out;
}

std::vector<morph::Piece> LatexPieces(FontStore& fonts, const tex::Layout& layout,
                                      const std::vector<std::string>& marks, Vector2 origin) {
  std::vector<morph::Piece> out;
  std::map<int, std::size_t> marked;   // mark -> its piece
  // The run of unmarked letters being gathered into a word, and where its next
  // letter would have to start to belong to it.
  int   run = -1;
  float runEnd = 0.0f;
  float runBaseline = 0.0f;
  for (const tex::Item& item : layout.items) {
    std::vector<std::vector<Vector2>> contours = tex::ItemContours(item, fonts);
    morph::Piece* piece = nullptr;
    if (item.mark > 0 && item.mark <= static_cast<int>(marks.size())) {
      run = -1;
      const auto found = marked.find(item.mark);
      if (found != marked.end()) {
        piece = &out[found->second];
      } else {
        marked[item.mark] = out.size();
        piece = &out.emplace_back();
        piece->key = "id:" + marks[static_cast<std::size_t>(item.mark - 1)];
      }
    } else if (item.kind == tex::Item::Kind::Glyph && IsLetter(item.codepoint) &&
               item.face == Face::MainRegular) {
      // Upright letters side by side are one word, `\sin`; italic ones are each
      // a variable of their own, `ab` being a times b.
      const bool continues = run >= 0 && std::fabs(item.pen.x - runEnd) < 0.05f * item.emPx &&
                             item.pen.y == runBaseline;
      if (continues) {
        piece = &out[static_cast<std::size_t>(run)];
        piece->key += static_cast<char>(item.codepoint);
      } else {
        run = static_cast<int>(out.size());
        piece = &out.emplace_back();
        piece->key = "id:" + std::string(1, static_cast<char>(item.codepoint));
      }
      runEnd = item.pen.x + fonts.Advance(item.face, item.codepoint, item.emPx);
      runBaseline = item.pen.y;
    } else {
      run = -1;
      piece = &out.emplace_back();
      switch (item.kind) {
        case tex::Item::Kind::Glyph:
          // An italic letter nobody marked -- LaTeX written as it stands -- is
          // still the variable of that name.
          piece->key = IsLetter(item.codepoint)
                           ? "id:" + std::string(1, static_cast<char>(item.codepoint))
                           : "g:" + std::to_string(item.codepoint);
          break;
        case tex::Item::Kind::Rect: piece->key = "rule"; break;
        case tex::Item::Kind::Line: piece->key = "line"; break;
        case tex::Item::Kind::Stroke: piece->key = "stroke:" + std::to_string(item.codepoint); break;
      }
    }
    for (const std::vector<Vector2>& contour : contours) {
      piece->contours.push_back(Placed(contour, origin));
    }
  }
  return out;
}

// ------------------------------------------------------------------- filling

namespace {

const char* kFillVertex = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragPos;
out vec4 fragColor;
void main() {
  fragPos = vertexPosition.xy;
  fragColor = vertexColor;
  gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

// A row of `points` is one shape: x and y in pixels, and in z the index its
// contour starts at, which is where the contour's last point joins back to.
const char* kFillFragment = R"(#version 330
in vec2 fragPos;
in vec4 fragColor;
uniform sampler2D points;
uniform int row;
uniform int count;
out vec4 finalColor;
void main() {
  int   winding = 0;
  float nearest = 1e9;
  for (int i = 0; i < count; ++i) {
    vec4 here = texelFetch(points, ivec2(i, row), 0);
    vec4 there = (i + 1 < count) ? texelFetch(points, ivec2(i + 1, row), 0) : vec4(-1.0);
    if (there.z != here.z) there = texelFetch(points, ivec2(int(here.z), row), 0);
    vec2  a = here.xy, e = there.xy - a, w = fragPos - a;
    float h = clamp(dot(w, e) / max(dot(e, e), 1e-12), 0.0, 1.0);
    nearest = min(nearest, length(w - e * h));
    float side = e.x * w.y - e.y * w.x;
    if (a.y <= fragPos.y) {
      if (there.y > fragPos.y && side > 0.0) ++winding;
    } else if (there.y <= fragPos.y && side < 0.0) {
      --winding;
    }
  }
  // Coverage, not in-or-out: how far the pixel's centre is from the edge.
  float cover = (winding != 0) ? clamp(0.5 + nearest, 0.0, 1.0) : clamp(0.5 - nearest, 0.0, 1.0);
  finalColor = vec4(fragColor.rgb, fragColor.a * cover);
}
)";

}  // namespace

bool OutlineFill::Load() {
  shader_ = LoadShaderFromMemory(kFillVertex, kFillFragment);
  if (!IsShaderValid(shader_)) return false;
  rowLoc_ = GetShaderLocation(shader_, "row");
  countLoc_ = GetShaderLocation(shader_, "count");
  pointsLoc_ = GetShaderLocation(shader_, "points");
  return true;
}

void OutlineFill::Unload() {
  if (points_.id != 0) UnloadTexture(points_);
  points_ = {};
  if (shader_.id != 0) UnloadShader(shader_);
  shader_ = {};
}

void OutlineFill::Draw(const std::vector<morph::Shape>& shapes, Color ink) {
  if (shader_.id == 0 || shapes.empty()) return;
  int wide = 1;
  for (const morph::Shape& shape : shapes) {
    int n = 0;
    for (const morph::Contour& c : shape.contours) n += static_cast<int>(c.size());
    wide = std::max(wide, n);
  }
  const int tall = static_cast<int>(shapes.size());
  if (wide > points_.width || tall > points_.height) {
    if (points_.id != 0) UnloadTexture(points_);
    points_.width = std::max(wide, points_.width);
    points_.height = std::max(tall, points_.height);
    points_.mipmaps = 1;
    points_.format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32;
    points_.id = rlLoadTexture(nullptr, points_.width, points_.height, points_.format, 1);
  }
  buffer_.assign(static_cast<std::size_t>(points_.width) * points_.height * 4, 0.0f);
  std::vector<Rectangle> bounds(shapes.size());
  std::vector<int>       counts(shapes.size());
  for (std::size_t r = 0; r < shapes.size(); ++r) {
    float* row = buffer_.data() + r * static_cast<std::size_t>(points_.width) * 4;
    int    n = 0;
    float  x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const morph::Contour& c : shapes[r].contours) {
      const int start = n;
      for (const morph::Point& p : c) {
        row[n * 4 + 0] = p.x;
        row[n * 4 + 1] = p.y;
        row[n * 4 + 2] = static_cast<float>(start);
        ++n;
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
      }
    }
    counts[r] = n;
    bounds[r] = {std::floor(x0) - 2.0f, std::floor(y0) - 2.0f, std::ceil(x1 - x0) + 5.0f,
                 std::ceil(y1 - y0) + 5.0f};
  }
  UpdateTexture(points_, buffer_.data());

  // One shape a batch: its row and count are uniforms, which apply to whatever
  // is drawn when the batch goes, so each goes before the next is set.
  rlDrawRenderBatchActive();
  BeginShaderMode(shader_);
  for (std::size_t r = 0; r < shapes.size(); ++r) {
    if (counts[r] < 3) continue;
    const int row = static_cast<int>(r);
    SetShaderValue(shader_, rowLoc_, &row, SHADER_UNIFORM_INT);
    SetShaderValue(shader_, countLoc_, &counts[r], SHADER_UNIFORM_INT);
    SetShaderValueTexture(shader_, pointsLoc_, points_);
    DrawRectangleRec(bounds[r], Fade(ink, shapes[r].alpha));
    rlDrawRenderBatchActive();
  }
  EndShaderMode();
}

// ------------------------------------------------------------------- the window

namespace {

// One side of a transition, laid out: its pieces, and the card round them.
struct Side {
  std::vector<morph::Piece> pieces;
  Rectangle                 card{};
  bool                      code = false;
};

struct Reading {
  std::string              latex;
  std::vector<std::string> marks;
};

// `$...$` is LaTeX as it stands; anything else is a line of code, read.
bool Read(const std::string& side, Reading* out) {
  if (side.size() >= 2 && side.front() == '$' && side.back() == '$') {
    out->latex = side.substr(1, side.size() - 2);
    return true;
  }
  const mathline::Line line = mathline::Parse(side);
  if (line.latex.empty()) return false;
  out->latex = line.latex;
  out->marks = line.marks;
  return true;
}

Rectangle Around(float x, float width, float baseline, float above, float below, float pad) {
  return {x - pad, baseline - above - pad, width + 2.0f * pad, above + below + 2.0f * pad};
}

Rectangle Mix(Rectangle a, Rectangle b, float s) {
  return {a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s, a.width + (b.width - a.width) * s,
          a.height + (b.height - a.height) * s};
}

Color Mix(Color a, Color b, float s) {
  const auto c = [s](unsigned char p, unsigned char q) {
    return static_cast<unsigned char>(std::lround(p + (q - p) * s));
  };
  return {c(a.r, b.r), c(a.g, b.g), c(a.b, b.b), c(a.a, b.a)};
}

}  // namespace

int Run(const std::string& assetDir, const std::vector<std::string>& sides,
        const std::string& screenshot, int frames, void (*saveFrame)(const std::string&)) {
  FontStore   fonts;
  std::string error;
  if (!fonts.Load(assetDir + "/fonts", &error)) {
    std::fprintf(stderr, "sketchbook: %s\n", error.c_str());
    return 1;
  }
  OutlineFill fill;
  if (!fill.Load()) {
    std::fprintf(stderr, "sketchbook: the outline shader did not compile\n");
    return 1;
  }

  const float W = static_cast<float>(GetScreenWidth());
  const float H = static_cast<float>(GetScreenHeight());
  // Laid out once at a trial size, then again at the size that makes the wider
  // side most of the window's width.
  const auto build = [&](float em, Side* a, Side* b) -> bool {
    const float codeEm = em * 0.8f;
    std::vector<Side*> out{a, b};
    for (std::size_t k = 0; k < 2; ++k) {
      Side& side = *out[k];
      side = {};
      const bool code = sides.size() == 1 && k == 0;
      if (code) {
        const float width = text::Measure(fonts, Face::Mono, sides[0], codeEm);
        const float above = text::Ascent(fonts, Face::Mono, codeEm);
        const float below = text::Descent(fonts, Face::Mono, codeEm);
        const Vector2 pen = {(W - width) * 0.5f, H * 0.5f + (above - below) * 0.5f};
        side.pieces = CodePieces(fonts, sides[0], codeEm, pen);
        side.card = Around(pen.x, width, pen.y, above, below, 0.6f * codeEm);
        side.code = true;
        continue;
      }
      Reading reading;
      const std::string& source = sides[std::min(k, sides.size() - 1)];
      if (!Read(source, &reading)) {
        std::fprintf(stderr, "sketchbook: \"%s\" has no LaTeX reading\n", source.c_str());
        return false;
      }
      const tex::Layout layout = tex::Render(reading.latex, fonts, em, /*display=*/false);
      if (!layout.error.empty()) {
        std::fprintf(stderr, "sketchbook: %s\n", layout.error.c_str());
      }
      const Vector2 origin = {(W - layout.width) * 0.5f,
                              H * 0.5f + (layout.height - layout.depth) * 0.5f};
      side.pieces = LatexPieces(fonts, layout, reading.marks, origin);
      side.card = Around(origin.x, layout.width, origin.y, layout.height, layout.depth, 0.6f * em);
    }
    return true;
  };
  Side from, to;
  if (!build(100.0f, &from, &to)) return 1;
  const float widest = std::max(from.card.width, to.card.width);
  const float tallest = std::max(from.card.height, to.card.height);
  const float em = 100.0f * std::min(0.8f * W / widest, 0.5f * H / tallest);
  build(em, &from, &to);

  const morph::Morph there(from.pieces, to.pieces);
  const morph::Morph back(to.pieces, from.pieces);
  std::printf("transition: %d pieces matched, %d fade\n", there.Matched(), there.Unmatched());

  constexpr double kHold = 0.7;
  constexpr double kMove = 1.5;
  constexpr int    kLegs = 6;   // three times there and back
  const double     total = kHold + kLegs * (kMove + kHold);
  int              frame = 0;
  double           clock = 0.0;
  while (!WindowShouldClose() && clock < total) {
    // Which leg, and how far through its move.
    const double into = clock - kHold;
    const int    leg = std::clamp(static_cast<int>(std::floor(into / (kMove + kHold))), 0, kLegs - 1);
    const double inLeg = into - leg * (kMove + kHold);
    const float  t = into < 0.0 ? 0.0f : static_cast<float>(std::clamp(inLeg / kMove, 0.0, 1.0));
    const bool   forward = leg % 2 == 0;
    const morph::Morph& morph = forward ? there : back;
    const Side& a = forward ? from : to;
    const Side& b = forward ? to : from;
    const float s = morph::Ease(t);

    BeginDrawing();
    ClearBackground(kCanvasBg);
    const Color paperA = a.code ? kCodeBg : kMathBg;
    const Color paperB = b.code ? kCodeBg : kMathBg;
    const Color inkA = a.code ? kCodeFg : kMathFg;
    const Color inkB = b.code ? kCodeFg : kMathFg;
    // Code is light on dark and LaTeX dark on light, so ink and paper fading
    // together would pass through the same grey. The paper turns over the middle
    // of the move, and the ink is whichever reads on it.
    const Color paper = Mix(paperA, paperB, morph::Ease((t - 0.35f) / 0.3f));
    const auto  contrast = [&paper](Color ink) {
      return std::abs((ink.r + ink.g + ink.b) - (paper.r + paper.g + paper.b));
    };
    DrawCard(Mix(a.card, b.card, s), 1.6f, paper, paper, 0.0f);
    fill.Draw(morph.At(t), contrast(inkA) >= contrast(inkB) ? inkA : inkB);
    ++frame;
    if (!screenshot.empty() && frame == frames && saveFrame != nullptr) saveFrame(screenshot);
    EndDrawing();
    if (frames > 0 && frame >= frames) break;

    clock += frames > 0 ? 1.0 / 60.0 : static_cast<double>(GetFrameTime());
  }
  fill.Unload();
  fonts.Unload();
  return 0;
}

}  // namespace sb::transition
