#include "blocks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "highlight.h"
#include "rlgl.h"
#include "text.h"
#include "theme.h"
#include "types/examples.h"

namespace sb {
namespace {

// Card geometry, in world units so it scales with the canvas.
constexpr float kPadX = 18.0f;
constexpr float kPadY = 14.0f;
constexpr float kRadius = 8.0f;
// The type size those numbers were chosen against. Padding is kept in
// proportion to it so that resizing a box is genuinely uniform: at the default
// size nothing has moved, and at any other size the whole card has scaled.
constexpr float kBaseEm = 26.0f;

// An empty box still needs to be big enough to see and click.
constexpr int kMinColumns = 14;

// The result square, to the right of the card.
constexpr float kSquareSide = 56.0f;
constexpr float kSquareGap = 20.0f;
// A shader preview goes in the same place, but a shader deserves more room.
constexpr float kShaderSide = 220.0f;

// The error balloon.
constexpr float kErrorEmScale = 0.75f;   // of the code size, as specified
constexpr float kErrorMaxWidth = 330.0f; // world units
constexpr float kErrorPad = 10.0f;

// The button above a focused box.
constexpr float kChromeHeight = 22.0f;
constexpr float kChromeGap = 7.0f;
constexpr float kChromeEm = 14.0f;
constexpr float kToggleWidth = 58.0f;
// The gap between one control and the next in a text box's strip.
constexpr float kChromeSpacing = 4.0f;

// A text box with no measure chosen yet gets one this many ems wide. About
// sixty characters, which is the width prose has been set at for five hundred
// years and is still about right.
constexpr float kMeasureEms = 20.0f;

// The bar just inside the right edge that sets the measure.
constexpr float kMarginBar = 4.0f;      // screen pixels wide
constexpr float kMarginGrab = 14.0f;    // and this much of a target

// Monospaced cells kept clear for an injected initialiser, so the box does not
// resize as the value changes width.
constexpr float kInjectionColumns = 13.0f;

// Keystrokes of the same kind within this long collapse into one undo step.
constexpr double kUndoCoalesceSeconds = 0.7;

// True for a UTF-8 continuation byte.
bool IsContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

int PrevBoundary(const std::string& s, int index) {
  if (index <= 0) return 0;
  --index;
  while (index > 0 && IsContinuation(s[static_cast<std::size_t>(index)])) --index;
  return index;
}

int NextBoundary(const std::string& s, int index) {
  const int size = static_cast<int>(s.size());
  if (index >= size) return size;
  ++index;
  while (index < size && IsContinuation(s[static_cast<std::size_t>(index)])) ++index;
  return index;
}

bool IsWordChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return std::isalnum(u) != 0 || c == '_' || u >= 0x80;
}

std::string EncodeUtf8(int codepoint) {
  std::string out;
  if (codepoint < 0x80) {
    out += static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    out += static_cast<char>(0xC0 | (codepoint >> 6));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    out += static_cast<char>(0xE0 | (codepoint >> 12));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (codepoint >> 18));
    out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  }
  return out;
}

bool CtrlDown() { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }

// Key repeat, so holding backspace behaves like every other editor.
bool Triggered(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }

std::string FormatInjected(float value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.4g", static_cast<double>(value));
  return buffer;
}

// raylib reports a shader's compile errors through TraceLog and nowhere else,
// so the log is caught here for the length of the call. Not re-entrant, and
// does not need to be: shaders are compiled one at a time, on the main thread.
std::string* g_shaderLog = nullptr;

void CaptureTraceLog(int level, const char* text, va_list args) {
  if (g_shaderLog == nullptr || level < LOG_WARNING) return;
  char buffer[2048];
  std::vsnprintf(buffer, sizeof(buffer), text, args);
  *g_shaderLog += buffer;
  *g_shaderLog += '\n';
}

bool CompileFragmentShader(const std::string& source, Shader* shader, std::string* log) {
  log->clear();
  g_shaderLog = log;
  SetTraceLogCallback(CaptureTraceLog);
  // A null vertex shader means raylib's own, which is what supplies
  // fragTexCoord and fragColor.
  const Shader compiled = LoadShaderFromMemory(nullptr, source.c_str());
  SetTraceLogCallback(nullptr);
  g_shaderLog = nullptr;

  if (IsShaderValid(compiled)) {
    *shader = compiled;
    return true;
  }
  UnloadShader(compiled);
  return false;
}

void DrawCard(Rectangle rect, float zoom, Color fill, Color border, float borderWidth) {
  const float radius = kRadius * zoom;
  const float minSide = std::max(1.0f, std::min(rect.width, rect.height));
  const float roundness = std::clamp(radius / (minSide * 0.5f), 0.0f, 1.0f);
  const int segments = std::clamp(static_cast<int>(radius * 0.8f), 4, 20);

  // A cheap stacked shadow: a few translucent offset copies read as a soft
  // drop shadow without a blur pass.
  for (int i = 3; i >= 1; --i) {
    const float spread = static_cast<float>(i) * 1.6f * zoom;
    const Rectangle s = {rect.x - spread, rect.y - spread + 2.0f * zoom,
                         rect.width + 2.0f * spread, rect.height + 2.0f * spread};
    Color c = kCardShadow;
    c.a = static_cast<unsigned char>(kCardShadow.a / (i * 2));
    DrawRectangleRounded(s, roundness, segments, c);
  }

  DrawRectangleRounded(rect, roundness, segments, fill);
  DrawRectangleRoundedLinesEx(rect, roundness, segments, borderWidth, border);
}

// Breaks a run of prose to a width, as byte ranges that **partition** it. The
// pieces meet exactly -- a trailing space stays with the piece it ends -- so
// every byte of the line, and therefore every position the caret can take, is
// in exactly one row.
void WrapProse(FontStore& fonts, Face face, const std::string& line, std::size_t from,
               float emPx, float maxWidth, std::vector<std::size_t>* begins,
               std::vector<std::size_t>* ends) {
  begins->clear();
  ends->clear();

  // Cumulative width at every character boundary, measured once.
  std::vector<std::size_t> at{from};
  std::vector<float>       cum{0.0f};
  for (std::size_t i = from; i < line.size();) {
    std::size_t next = i + 1;
    while (next < line.size() && (static_cast<unsigned char>(line[next]) & 0xC0) == 0x80) ++next;
    cum.push_back(cum.back() +
                  text::Measure(fonts, face, std::string_view(line).substr(i, next - i), emPx));
    at.push_back(next);
    i = next;
  }

  std::size_t start = 0;   // index into `at`
  while (start + 1 < at.size()) {
    if (cum.back() - cum[start] <= maxWidth) {
      begins->push_back(at[start]);
      ends->push_back(line.size());
      return;
    }
    // The furthest boundary that still fits.
    std::size_t fit = start + 1;
    while (fit + 1 < at.size() && cum[fit + 1] - cum[start] <= maxWidth) ++fit;
    // Back up to the last space, so words are not cut in half. A single word
    // too long for the measure is broken rather than allowed to overflow.
    std::size_t cut = fit;
    while (cut > start + 1 && line[at[cut] - 1] != ' ') --cut;
    if (cut <= start) cut = fit;
    begins->push_back(at[start]);
    ends->push_back(at[cut]);
    start = cut;
  }
  if (begins->empty()) {
    begins->push_back(from);
    ends->push_back(line.size());
  }
}

// Draws part of a line with its syntax colours. Returns the width drawn.
float DrawSpans(FontStore& fonts, std::string_view line,
                const std::vector<highlight::Span>& spans, std::size_t from, std::size_t to,
                Vector2 pen, float emPx) {
  float x = pen.x;
  for (const highlight::Span& span : spans) {
    const std::size_t begin = std::max(span.begin, from);
    const std::size_t end = std::min(span.end, to);
    if (end <= begin) continue;
    const std::string_view piece = line.substr(begin, end - begin);
    text::Draw(fonts, Face::Mono, piece, {x, pen.y}, emPx, span.color);
    x += text::Measure(fonts, Face::Mono, piece, emPx);
  }
  return x - pen.x;
}

}  // namespace

// ------------------------------------------------------------------- code
CodeBlock::CodeBlock(Vector2 worldTopLeft, float worldEm, std::vector<std::string> lines)
    : origin_(worldTopLeft), em_(worldEm), lines_(std::move(lines)) {
  if (lines_.empty()) lines_.emplace_back();
}

CodeBlock::~CodeBlock() {
  // Nothing is freed here on purpose: releasing a shader needs the graphics
  // context, and a destructor can run after the window has gone. Whoever
  // destroys a box has to have released it first. Say so if they did not,
  // rather than leaking in silence.
  if (haveShader_ || haveProbe_ || havePreviewSurface_ || haveProbeTarget_) {
    TraceLog(LOG_WARNING, "BLOCKS: a box was destroyed still holding GPU memory");
  }
}

void CodeBlock::EnsureCache(FontStore& fonts, float emPx, float measurePx) const {
  const bool prose = IsText();
  // Bold and italic change which face the words are set in, so they invalidate
  // a line as surely as editing it does.
  const unsigned styleSig = (bold_ ? 1u : 0u) | (italic_ ? 2u : 0u) | (prose ? 4u : 0u);

  cache_.resize(lines_.size());
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    LineCache& entry = cache_[i];
    if (entry.source != lines_[i] || entry.styleSig != styleSig) {
      entry.source = lines_[i];
      entry.styleSig = styleSig;
      // A text box has no mathematical reading and is not looked for one: its
      // lines are words, and words are not code that failed to parse.
      entry.parsed = prose ? mathline::Line{} : mathline::Parse(lines_[i]);
      entry.latex = prose ? std::string() : entry.parsed.latex;
      entry.tex = entry.latex.empty() ? tex::Parsed{} : tex::Parse(entry.latex);
      entry.emPx = -1.0f;
      entry.wrapWidth = -1.0f;
    }

    if (prose) {
      // Rebuilt when the words, the face, the size or the measure change -- and
      // not otherwise, since this lays out every row of the paragraph.
      if (entry.emPx == emPx && entry.wrapWidth == measurePx) continue;
      entry.emPx = emPx;
      entry.wrapWidth = measurePx;
      WrapProse(fonts, ProseFace(), lines_[i], ProseOffset(i), emPx, measurePx,
                &entry.pieceBegin, &entry.pieceEnd);
      entry.pieceLayout.assign(entry.pieceBegin.size(), tex::Layout{});
      for (std::size_t k = 0; k < entry.pieceBegin.size(); ++k) {
        const std::string_view words = std::string_view(lines_[i]).substr(
            entry.pieceBegin[k], entry.pieceEnd[k] - entry.pieceBegin[k]);
        entry.pieceLayout[k] = tex::Render(TextLatex(words), fonts, emPx, /*display=*/false);
      }
      continue;
    }

    if (entry.latex.empty()) continue;
    if (entry.emPx != emPx) {
      entry.emPx = emPx;
      // Text style, not display: these sit inline in a box, not on their own.
      entry.layout = tex::BuildLayout(entry.tex, fonts, emPx, /*display=*/false);
    }
  }
}

// ------------------------------------------------------------------- prose

bool CodeBlock::IsText() const {
  if (lines_.empty()) return false;
  const std::string& first = lines_[0];
  std::size_t i = 0;
  while (i < first.size() && first[i] == ' ') ++i;
  return first.size() - i >= 5 && first.compare(i, 5, "\\text") == 0;
}

std::size_t CodeBlock::ProseOffset(std::size_t line) const {
  if (line != 0 || lines_.empty()) return 0;
  const std::string& first = lines_[0];
  std::size_t i = 0;
  while (i < first.size() && first[i] == ' ') ++i;
  if (first.size() - i < 5 || first.compare(i, 5, "\\text") != 0) return 0;
  i += 5;
  // One space after the sigil is punctuation, not a word.
  if (i < first.size() && first[i] == ' ') ++i;
  return i;
}

std::string CodeBlock::Prose(std::size_t line) const {
  if (line >= lines_.size()) return {};
  return lines_[line].substr(ProseOffset(line));
}

Face CodeBlock::ProseFace() const {
  // There is no bold-italic cut of Computer Modern in the KaTeX set, so asking
  // for both gives bold.
  if (bold_) return Face::MainBold;
  if (italic_) return Face::MainItalic;
  return Face::MainRegular;
}

std::string CodeBlock::TextLatex(std::string_view words) const {
  // \text takes its contents raw, so everything reaches the page as itself and
  // one character stays one glyph -- which is what lets the caret be placed in
  // the words rather than in the source. Only the braces have to go, since they
  // are what tells the parser where the argument ends.
  std::string escaped;
  for (char c : words) escaped += (c == '{' || c == '}') ? ' ' : c;
  while (!escaped.empty() && escaped.back() == ' ') escaped.pop_back();
  if (escaped.empty()) return {};
  const char* command = bold_ ? "\\textbf" : (italic_ ? "\\textit" : "\\text");
  return std::string(command) + "{" + escaped + "}";
}

int CodeBlock::ErrorLine() const {
  if (program_.state() == mathprogram::State::Failed && program_.error().present) {
    return program_.error().line;
  }
  if (outcome_.status == compiler::Status::Failed && outcome_.error.present) {
    return outcome_.error.line;
  }
  return 0;
}

bool CodeBlock::ShowsCode(int line) const {
  if (line < 0 || line >= static_cast<int>(cache_.size())) return true;
  // A text box is words all the way through, and stays words while it is being
  // edited: what you are writing is what you can see. The one way to get at the
  // source -- and so at the sigil, which is how a text box stops being one --
  // is the toggle on its own strip.
  if (IsText()) return showSource_;
  if (cache_[static_cast<std::size_t>(line)].latex.empty()) return true;
  // A line the compiler complained about shows its code, so the red caret has
  // real characters to point at -- and so you can see what to fix.
  if (line + 1 == ErrorLine()) return true;
  // Editing the box means editing all of it, unless the toggle says otherwise.
  if (focused_ && !showMath_) return true;
  if (focused_ && line == caretLine_) return true;
  if (line == hoverLine_) return true;
  return false;
}

std::string CodeBlock::Injection(int line) const {
  if (line < 0 || line >= static_cast<int>(cache_.size())) return {};
  const mathline::Line& parsed = cache_[static_cast<std::size_t>(line)].parsed;
  if (parsed.kind != mathline::Kind::Declaration || parsed.hasInitialiser) return {};

  // A function's chosen value is a piece of code rather than a number, and
  // unlike a real's it does not move under a knob -- there is nothing to drag.
  if (mathline::IsFunctionType(parsed.type)) {
    return " = " + types::ExampleRealFunction(parsed.name);
  }

  for (const mathprogram::Variable& variable : program_.variables()) {
    if (variable.name == parsed.name) return " = " + FormatInjected(variable.value);
  }
  return " = " + FormatInjected(types::ExampleReal(parsed.name));
}

Color CodeBlock::VariableColor(const std::string& name) const {
  const std::vector<mathprogram::Variable>& variables = program_.variables();
  for (std::size_t i = 0; i < variables.size(); ++i) {
    if (variables[i].name == name) return sb::VariableColor(static_cast<int>(i));
  }
  // Something the box mentions but does not declare -- a shader's input, which
  // the pointer stands for. It takes the next colour along, so it is as
  // distinct from the box's own variables as they are from each other.
  return sb::VariableColor(static_cast<int>(variables.size()));
}

void CodeBlock::SetHighlights(std::vector<std::string> names) {
  highlights_ = std::move(names);
}

void CodeBlock::Hide(const std::string& name) {
  if (IsHidden(name)) return;
  hidden_.push_back(name);
}

void CodeBlock::Unhide(const std::string& name) {
  hidden_.erase(std::remove(hidden_.begin(), hidden_.end(), name), hidden_.end());
}

bool CodeBlock::IsHidden(const std::string& name) const {
  return std::find(hidden_.begin(), hidden_.end(), name) != hidden_.end();
}

bool CodeBlock::BakeVariable(const std::string& name) {
  const mathprogram::Variable* variable = FindVariable(name);
  if (variable == nullptr || !variable->free) return false;

  for (std::size_t i = 0; i < lines_.size(); ++i) {
    const mathline::Line parsed = mathline::Parse(lines_[i]);
    if (parsed.kind != mathline::Kind::Declaration || parsed.hasInitialiser) continue;
    if (parsed.name != name) continue;

    // The value that was only ever injected becomes the user's own text. It was
    // theirs to choose and they have chosen it.
    PushUndo(EditKind::Structural);
    lines_[i] = parsed.type + " " + parsed.name + " = " + FormatInjected(variable->value) + ";";
    TouchEdit();
    dirty_ = true;
    RequestCompile();
    return true;
  }
  return false;
}

void CodeBlock::DrawShaderInto(Rectangle rect) const {
  if (!haveShader_ || !havePreviewSurface_) return;

  for (const auto& [name, location] : uniforms_) {
    if (location < 0) continue;
    const mathprogram::Variable* variable = FindVariable(name);
    if (variable == nullptr) continue;
    const float value = variable->value;
    SetShaderValue(shader_, location, &value, SHADER_UNIFORM_FLOAT);
  }

  BeginShaderMode(shader_);
  // One white pixel stretched over the square: the source rectangle covers the
  // whole texture, so fragTexCoord runs 0..1 across it.
  DrawTexturePro(previewSurface_, {0.0f, 0.0f, 1.0f, 1.0f}, rect, {0.0f, 0.0f}, 0.0f, WHITE);
  EndShaderMode();
}

std::vector<Rectangle> CodeBlock::MentionRects(std::size_t rowIndex, const std::string& name,
                                               const Geometry& g, FontStore& fonts) const {
  std::vector<Rectangle> rects;
  if (rowIndex >= g.rows.size() || name.empty()) return rects;
  const Row&        row = g.rows[rowIndex];
  const std::size_t line = static_cast<std::size_t>(row.line);
  if (line >= cache_.size()) return rects;
  // Prose declares nothing, so nothing in it is a mention.
  if (IsText()) return rects;

  // A box a little taller than the letters, so it reads as a highlight rather
  // than as something touching the glyphs.
  // Tight horizontally: `ac` is two mentions side by side, and a generous box
  // round one of them would sit on top of the other.
  auto boxAround = [](float left, float right, float baseline, float em) {
    const float top = baseline - em * 0.80f;
    return Rectangle{left - em * 0.05f, top, (right - left) + em * 0.10f, em * 1.06f};
  };

  if (row.asLatex) {
    // mathline tagged every identifier it wrote; the tag survived typesetting,
    // so the box is simply what the tagged items cover.
    const std::vector<std::string>& marks = cache_[line].parsed.marks;
    for (std::size_t k = 0; k < marks.size(); ++k) {
      if (marks[k] != name) continue;
      const int tag = static_cast<int>(k) + 1;

      bool  any = false;
      float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
      for (const tex::Item& item : cache_[line].layout.items) {
        if (item.mark != tag) continue;
        float itemLeft = item.pen.x;
        float itemRight = item.pen.x;
        float itemTop = item.pen.y - item.emPx * 0.80f;
        float itemBottom = item.pen.y + item.emPx * 0.26f;
        if (item.kind == tex::Item::Kind::Glyph) {
          itemRight += fonts.Advance(item.face, item.codepoint, item.emPx);
        } else {
          itemLeft = item.rect.x;
          itemRight = item.rect.x + item.rect.width;
          itemTop = item.rect.y;
          itemBottom = item.rect.y + item.rect.height;
        }
        if (!any) {
          left = itemLeft; right = itemRight; top = itemTop; bottom = itemBottom;
          any = true;
          continue;
        }
        left = std::min(left, itemLeft);
        right = std::max(right, itemRight);
        top = std::min(top, itemTop);
        bottom = std::max(bottom, itemBottom);
      }
      if (!any) continue;

      // Item coordinates are relative to the formula's left edge and baseline.
      const float originX = row.x;
      rects.push_back({originX + left - g.emPx * 0.05f, row.baseline + top,
                       (right - left) + g.emPx * 0.10f, bottom - top});
    }
    return rects;
  }

  // Code is monospaced, so a column is just an offset.
  const std::string& source = lines_[line];
  auto isWordChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
  };
  for (std::size_t at = source.find(name); at != std::string::npos;
       at = source.find(name, at + 1)) {
    if (at < row.begin || at >= row.end) continue;
    const bool beforeOk = (at == 0) || !isWordChar(source[at - 1]);
    const std::size_t end = at + name.size();
    const bool afterOk = (end >= source.size()) || !isWordChar(source[end]);
    if (!beforeOk || !afterOk) continue;   // part of a longer name

    const float left = row.x + g.advance * static_cast<float>(at);
    const float right = left + g.advance * static_cast<float>(name.size());
    rects.push_back(boxAround(left, right, row.baseline, g.emPx));
  }
  return rects;
}

const CodeBlock::Geometry& CodeBlock::Measure(const Viewport& viewport, FontStore& fonts) const {
  Geometry& g = geometry_;
  g.zoom = viewport.Zoom();
  g.emPx = em_ * g.zoom;
  g.advance = fonts.Advance(Face::Mono, 'M', g.emPx);
  g.lineHeight = text::LineHeight(fonts, Face::Mono, g.emPx);
  g.ascent = text::Ascent(fonts, Face::Mono, g.emPx);

  // The measure, in pixels: how far the words run before they wrap. A box that
  // has never been given one takes a sensible measure from its own type size.
  g.measure = (textWidth_ > 0.0f ? textWidth_ : em_ * kMeasureEms) * g.zoom;
  EnsureCache(fonts, g.emPx, g.measure);

  // Padding is in proportion to the type, so growing a box grows the whole
  // card and not just what is written in it.
  const float padScale = em_ / kBaseEm;
  const float padX = kPadX * padScale * g.zoom;
  const float padY = kPadY * padScale * g.zoom;

  const Vector2 topLeft = viewport.ToScreen(origin_);
  float y = topLeft.y + padY;

  g.rows.clear();
  g.rows.reserve(lines_.size());
  float contentWidth = g.advance * static_cast<float>(kMinColumns);

  // A paragraph is exactly as wide as its measure -- whether or not any row
  // reaches it, and regardless of how long the source line behind it is. A
  // paragraph is a shape, and sizing it to its unwrapped source would make a
  // box thousands of units wide to hold four lines of words.
  //
  // Everything else is sized for the worst case across *both* readings, so it
  // never changes shape when a line flips or a value changes width.
  const bool showingWords = IsText() && !showSource_;
  if (showingWords) {
    contentWidth = std::max(contentWidth, g.measure);
  } else {
    for (std::size_t i = 0; i < lines_.size(); ++i) {
      float codeWidth = text::Measure(fonts, Face::Mono, lines_[i], g.emPx);
      const std::string injection = Injection(static_cast<int>(i));
      if (!injection.empty()) {
        // A real's injection is a number that changes width as its knob moves,
        // so it gets a fixed slot rather than the width of the moment --
        // otherwise the box would breathe in and out as you drag. A function's
        // never changes, so it gets exactly the room it needs.
        codeWidth += std::max(g.advance * kInjectionColumns,
                              text::Measure(fonts, Face::Mono, injection, g.emPx));
      }
      contentWidth = std::max(contentWidth, codeWidth);
    }
  }

  for (std::size_t i = 0; i < lines_.size(); ++i) {
    const bool asLatex = !ShowsCode(static_cast<int>(i));
    const bool wrapped = asLatex && IsText();

    // Prose wraps, so one line becomes as many rows as it takes.
    const std::size_t pieces =
        wrapped ? std::max<std::size_t>(cache_[i].pieceBegin.size(), 1) : 1;

    for (std::size_t k = 0; k < pieces; ++k) {
      Row row;
      row.asLatex = asLatex;
      row.line = static_cast<int>(i);
      row.piece = static_cast<int>(k);
      row.y = y;

      if (wrapped && k < cache_[i].pieceBegin.size()) {
        row.begin = cache_[i].pieceBegin[k];
        row.end = cache_[i].pieceEnd[k];
      } else if (wrapped) {
        row.begin = ProseOffset(i);
        row.end = lines_[i].size();
      } else {
        row.begin = 0;
        row.end = lines_[i].size();
      }

      const tex::Layout* layout = nullptr;
      if (wrapped) {
        if (k < cache_[i].pieceLayout.size()) layout = &cache_[i].pieceLayout[k];
      } else if (asLatex && !cache_[i].latex.empty()) {
        layout = &cache_[i].layout;
      }

      float latexExtent = 0.0f;
      if (layout != nullptr) {
        row.width = layout->width;
        latexExtent = layout->height + layout->depth + 6.0f * g.zoom;
        contentWidth = std::max(contentWidth, row.width);
        row.baseline = 0.0f;
      } else {
        row.width = text::Measure(fonts, Face::Mono, lines_[i], g.emPx);
      }

      row.height = std::max(g.lineHeight, latexExtent);
      if (layout != nullptr) {
        row.baseline =
            row.y + (row.height - (layout->height + layout->depth)) * 0.5f + layout->height;
      } else {
        row.baseline = row.y + (row.height - g.lineHeight) * 0.5f + g.ascent;
      }
      y += row.height;
      g.rows.push_back(row);
    }
  }

  const float contentHeight = y - (topLeft.y + padY);
  g.rect = {topLeft.x, topLeft.y, contentWidth + 2.0f * padX, contentHeight + 2.0f * padY};
  g.textLeftTop = {topLeft.x + padX, topLeft.y + padY};

  // Justification is a second pass, because it is about where a row sits in the
  // box and the box's width is not settled until every row has been measured.
  // Code is always flush left: a column means something in code and nothing in
  // prose.
  for (Row& row : g.rows) {
    const float slack = std::max(0.0f, contentWidth - row.width);
    float offset = 0.0f;
    if (row.asLatex && IsText() && justify_ == Justify::Centre) offset = slack * 0.5f;
    if (row.asLatex && IsText() && justify_ == Justify::Right) offset = slack;
    row.x = g.textLeftTop.x + offset;
  }
  return g;
}

float CodeBlock::PaddingX(float em) { return kPadX * (em / kBaseEm); }

Rectangle CodeBlock::WorldBounds(const Viewport& viewport, FontStore& fonts) const {
  const Geometry& g = Measure(viewport, fonts);
  const float     zoom = std::max(viewport.Zoom(), 1e-6f);
  return {origin_.x, origin_.y, g.rect.width / zoom, g.rect.height / zoom};
}

void CodeBlock::Scale(float factor, Vector2 anchorWorld) {
  const float wanted = std::clamp(em_ * factor, 3.0f, 800.0f);
  const float applied = wanted / em_;
  // The anchor is the corner the pointer is nearest, so the box grows away
  // from the hand rather than out from under it.
  origin_ = {anchorWorld.x + (origin_.x - anchorWorld.x) * applied,
             anchorWorld.y + (origin_.y - anchorWorld.y) * applied};
  em_ = wanted;
  // The measure goes with the type, or the resize would reflow the paragraph
  // instead of scaling it.
  if (textWidth_ > 0.0f) textWidth_ *= applied;
}

CodeBlock::Chrome CodeBlock::ChromeRects(const Geometry& g) const {
  const float height = kChromeHeight * g.zoom;
  const float y = g.rect.y - height - kChromeGap * g.zoom;

  Chrome chrome;
  if (IsText()) {
    // Bold, italic, then the three justifications: squares of the strip's own
    // height, so the strip is one row however many controls end up in it.
    chrome.forText = true;
    const float side = height;
    const float gap = kChromeSpacing * g.zoom;
    float       x = g.rect.x;
    chrome.bold = {x, y, side, side};
    x += side + gap;
    chrome.italic = {x, y, side, side};
    x += side + gap * 3.0f;   // a break, because the next three are one choice
    for (int i = 0; i < 3; ++i) {
      chrome.justify[i] = {x, y, side, side};
      x += side + gap;
    }
    // Then the same control a code box has: show me what this really is. For
    // prose that means the source, sigil and all -- which is the only way to
    // take the sigil back out.
    //
    // It stays with the rest of the strip rather than going to the box's right
    // edge, because showing the source makes a wrapped paragraph as wide as its
    // longest unwrapped line, and a toggle that leaves the screen when pressed
    // is a toggle you cannot press again.
    chrome.toggle = {x + gap * 2.0f, y, kToggleWidth * g.zoom, height};
    return chrome;
  }

  const float toggleWidth = kToggleWidth * g.zoom;
  // The cross that used to sit here is gone: a box is thrown away the same way
  // every other window is, by carrying it onto the drop target.
  chrome.toggle = {g.rect.x + g.rect.width - toggleWidth, y, toggleWidth, height};
  return chrome;
}

bool CodeBlock::ChromeVisible(const Geometry& g, Vector2 pointer) const {
  if (focused_) return true;
  // Only a text box shows controls without the caret: they are how its words
  // are set, which is a thing you want to change without starting to type.
  if (!IsText() || absorbed_) return false;
  const float     height = kChromeHeight * g.zoom;
  const float     lead = kChromeGap * g.zoom;
  const Rectangle band = {g.rect.x, g.rect.y - height - lead * 2.0f,
                          std::max(g.rect.width, height * 8.0f), height + lead * 2.0f};
  return CheckCollisionPointRec(pointer, band) || CheckCollisionPointRec(pointer, g.rect) ||
         PointOnRim(g.rect, pointer);
}

Rectangle CodeBlock::MarginHandle(const Geometry& g) const {
  // A short bar just inside the right edge, at the middle: it *is* the right
  // margin, which is what dragging it sets. Inset clear of the rim's grab band,
  // so carrying the box and setting its measure never want the same pixel.
  const float inset = kRimGrabInside;
  const float height = std::min(g.rect.height * 0.4f, 34.0f);
  return {g.rect.x + g.rect.width - inset - kMarginGrab,
          g.rect.y + (g.rect.height - height) * 0.5f, kMarginGrab, height};
}

Vector2 CodeBlock::DockAnchor(const Viewport& viewport, FontStore& fonts) const {
  const Geometry& g = Measure(viewport, fonts);
  return {g.rect.x, g.rect.y + g.rect.height};
}

float CodeBlock::DockWidth(const Viewport& viewport, FontStore& fonts) const {
  return Measure(viewport, fonts).rect.width;
}

bool CodeBlock::Contains(Vector2 point, const Viewport& viewport, FontStore& fonts) const {
  return CheckCollisionPointRec(point, Measure(viewport, fonts).rect);
}

bool CodeBlock::OnRim(Vector2 point, const Viewport& viewport, FontStore& fonts) const {
  const Geometry& g = Measure(viewport, fonts);
  return PointOnRim(g.rect, point);
}

int CodeBlock::RowAt(Vector2 point, const Viewport& viewport, FontStore& fonts) const {
  const Geometry& g = Measure(viewport, fonts);
  if (!CheckCollisionPointRec(point, g.rect)) return -1;
  for (std::size_t i = 0; i < g.rows.size(); ++i) {
    if (point.y >= g.rows[i].y && point.y < g.rows[i].y + g.rows[i].height) {
      return static_cast<int>(i);
    }
  }
  return g.rows.empty() ? -1 : static_cast<int>(g.rows.size()) - 1;
}

int CodeBlock::CaretRow(const Geometry& g) const {
  int fallback = -1;
  for (std::size_t i = 0; i < g.rows.size(); ++i) {
    const Row& row = g.rows[i];
    if (row.line != caretLine_) continue;
    if (fallback < 0) fallback = static_cast<int>(i);
    // The first row that contains the offset wins, so a caret sitting exactly
    // on a wrap point shows at the end of a line rather than the start of the
    // next -- which is where you just typed.
    if (static_cast<std::size_t>(caretCol_) >= row.begin &&
        static_cast<std::size_t>(caretCol_) <= row.end) {
      return static_cast<int>(i);
    }
    fallback = static_cast<int>(i);
  }
  return fallback;
}

// How far into a row a byte offset sits, and the reverse. Both are read off the
// cached layout rather than measured, so that the arrow keys -- which are
// handled where there is no font store -- can use them too.
float CodeBlock::RowOffsetX(const Row& row, std::size_t col) const {
  if (!row.asLatex || !IsText()) return 0.0f;
  const LineCache& entry = cache_[static_cast<std::size_t>(row.line)];
  if (static_cast<std::size_t>(row.piece) >= entry.pieceLayout.size()) return 0.0f;
  const tex::Layout& layout = entry.pieceLayout[static_cast<std::size_t>(row.piece)];

  const std::string& line = lines_[static_cast<std::size_t>(row.line)];
  std::size_t        glyph = 0;
  for (std::size_t i = row.begin; i < col && i < row.end; ++glyph) {
    i = static_cast<std::size_t>(NextBoundary(line, static_cast<int>(i)));
  }
  if (glyph >= layout.items.size()) return layout.width;
  return layout.items[glyph].pen.x;
}

std::size_t CodeBlock::RowColAt(const Row& row, float x) const {
  const std::string& line = lines_[static_cast<std::size_t>(row.line)];
  std::size_t        best = row.begin;
  float              bestDistance = std::abs(x);
  for (std::size_t i = row.begin; i <= row.end;) {
    const float distance = std::abs(x - RowOffsetX(row, i));
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
    if (i >= row.end) break;
    i = static_cast<std::size_t>(NextBoundary(line, static_cast<int>(i)));
  }
  return best;
}

void CodeBlock::PlaceCaret(Vector2 point, const Viewport& viewport, FontStore& fonts) {
  const Geometry& g = Measure(viewport, fonts);
  if (g.rows.empty()) return;
  const int found = RowAt(point, viewport, fonts);
  const Row& row = g.rows[static_cast<std::size_t>(
      std::clamp(found < 0 ? 0 : found, 0, static_cast<int>(g.rows.size()) - 1))];
  caretLine_ = row.line;

  // Walk the row and take the character boundary nearest the click. Prose is
  // measured in its own face; code in the monospaced one.
  const std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  const Face  face = row.asLatex ? ProseFace() : Face::Mono;
  const float target = point.x - row.x;

  std::size_t best = row.begin;
  float       bestDistance = std::abs(target);
  float       x = 0.0f;
  for (std::size_t i = row.begin; i < row.end;) {
    const std::size_t next = static_cast<std::size_t>(NextBoundary(line, static_cast<int>(i)));
    x += text::Measure(fonts, face, std::string_view(line).substr(i, next - i), g.emPx);
    const float distance = std::abs(target - x);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = next;
    }
    i = next;
  }
  caretCol_ = static_cast<int>(best);
  ClampCaret();
  blinkAnchor_ = GetTime();
}

void CodeBlock::SetFocused(bool focused) {
  focused_ = focused;
  blinkAnchor_ = GetTime();
  lastEdit_ = EditKind::None;   // a fresh visit starts a fresh undo step
  if (focused) ClampCaret();
}

bool CodeBlock::Empty() const {
  for (const std::string& line : lines_) {
    if (!line.empty()) return false;
  }
  return true;
}

std::string CodeBlock::Text() const {
  std::string out;
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (i > 0) out += '\n';
    out += lines_[i];
  }
  return out;
}

const mathprogram::Variable* CodeBlock::FindVariable(const std::string& name) const {
  for (const mathprogram::Variable& variable : program_.variables()) {
    if (variable.name == name) return &variable;
  }
  return nullptr;
}

void CodeBlock::SetVariableValue(const std::string& name, float value) {
  const std::vector<mathprogram::Variable>& variables = program_.variables();
  for (std::size_t i = 0; i < variables.size(); ++i) {
    if (variables[i].name == name) {
      program_.SetValue(i, value);
      return;
    }
  }
}

void CodeBlock::RestoreValues(std::vector<std::pair<std::string, float>> values) {
  program_.Restore(std::move(values));
}

void CodeBlock::PushUndo(EditKind kind) {
  const double now = GetTime();
  const bool sameRun = (kind == lastEdit_) && (now - lastEditTime_ < kUndoCoalesceSeconds);
  lastEdit_ = kind;
  lastEditTime_ = now;
  if (sameRun && !undo_.empty()) return;   // keep typing as one step

  undo_.push_back({lines_, caretLine_, caretCol_});
  if (undo_.size() > 200) undo_.erase(undo_.begin());
}

void CodeBlock::Undo() {
  if (undo_.empty()) return;
  const Snapshot snapshot = undo_.back();
  undo_.pop_back();
  lines_ = snapshot.lines;
  caretLine_ = snapshot.caretLine;
  caretCol_ = snapshot.caretCol;
  if (lines_.empty()) lines_.emplace_back();
  ClampCaret();
  dirty_ = true;
  lastEdit_ = EditKind::None;
  blinkAnchor_ = GetTime();
}

void CodeBlock::TouchEdit() {
  dirty_ = true;
  blinkAnchor_ = GetTime();
}

void CodeBlock::ClampCaret() {
  caretLine_ = std::clamp(caretLine_, 0, static_cast<int>(lines_.size()) - 1);
  std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  // While the words are what is showing, the sigil is not: the caret cannot go
  // into something that is not on screen.
  const int floorCol = (IsText() && !showSource_)
                           ? static_cast<int>(ProseOffset(static_cast<std::size_t>(caretLine_)))
                           : 0;
  caretCol_ = std::clamp(caretCol_, floorCol, static_cast<int>(line.size()));
  while (caretCol_ > 0 && caretCol_ < static_cast<int>(line.size()) &&
         IsContinuation(line[static_cast<std::size_t>(caretCol_)])) {
    --caretCol_;
  }
}

void CodeBlock::InsertCodepoint(int codepoint) {
  PushUndo(EditKind::Insert);
  const std::string encoded = EncodeUtf8(codepoint);
  std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  line.insert(static_cast<std::size_t>(caretCol_), encoded);
  caretCol_ += static_cast<int>(encoded.size());
  TouchEdit();
}

void CodeBlock::InsertNewline() {
  PushUndo(EditKind::Structural);
  std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  std::string tail = line.substr(static_cast<std::size_t>(caretCol_));

  // Carry the current line's indentation onto the new one.
  std::string indent;
  for (char c : line) {
    if (c == ' ' || c == '\t') indent += c;
    else break;
  }
  if (static_cast<std::size_t>(caretCol_) < indent.size()) {
    indent.resize(static_cast<std::size_t>(caretCol_));
  }

  line.erase(static_cast<std::size_t>(caretCol_));
  lines_.insert(lines_.begin() + caretLine_ + 1, indent + tail);
  ++caretLine_;
  caretCol_ = static_cast<int>(indent.size());
  TouchEdit();
}

void CodeBlock::DeleteBefore() {
  std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  if (caretCol_ > 0) {
    PushUndo(EditKind::Delete);
    const int start = PrevBoundary(line, caretCol_);
    line.erase(static_cast<std::size_t>(start), static_cast<std::size_t>(caretCol_ - start));
    caretCol_ = start;
  } else if (caretLine_ > 0) {
    PushUndo(EditKind::Structural);
    std::string tail = line;
    lines_.erase(lines_.begin() + caretLine_);
    --caretLine_;
    caretCol_ = static_cast<int>(lines_[static_cast<std::size_t>(caretLine_)].size());
    lines_[static_cast<std::size_t>(caretLine_)] += tail;
  } else {
    return;
  }
  TouchEdit();
}

void CodeBlock::DeleteAt() {
  std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  if (caretCol_ < static_cast<int>(line.size())) {
    PushUndo(EditKind::Delete);
    const int end = NextBoundary(line, caretCol_);
    line.erase(static_cast<std::size_t>(caretCol_), static_cast<std::size_t>(end - caretCol_));
  } else if (caretLine_ + 1 < static_cast<int>(lines_.size())) {
    PushUndo(EditKind::Structural);
    line += lines_[static_cast<std::size_t>(caretLine_ + 1)];
    lines_.erase(lines_.begin() + caretLine_ + 1);
  } else {
    return;
  }
  TouchEdit();
}

void CodeBlock::MoveCaret(int dLine, int dChar) {
  if (dChar != 0) {
    const std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
    if (dChar < 0) {
      if (caretCol_ > 0) {
        caretCol_ = PrevBoundary(line, caretCol_);
      } else if (caretLine_ > 0) {
        --caretLine_;
        caretCol_ = static_cast<int>(lines_[static_cast<std::size_t>(caretLine_)].size());
      }
    } else {
      if (caretCol_ < static_cast<int>(line.size())) {
        caretCol_ = NextBoundary(line, caretCol_);
      } else if (caretLine_ + 1 < static_cast<int>(lines_.size())) {
        ++caretLine_;
        caretCol_ = 0;
      }
    }
  }
  if (dLine != 0) {
    // In prose a line is not a row: a wrapped paragraph is one line and many
    // rows, and the arrow keys mean the rows. The geometry from the last frame
    // is what says where those rows are; there is no font store out here.
    const int from = (IsText() && !showSource_) ? CaretRow(geometry_) : -1;
    if (from >= 0) {
      const int to = std::clamp(from + dLine, 0, static_cast<int>(geometry_.rows.size()) - 1);
      if (to != from) {
        const Row& source = geometry_.rows[static_cast<std::size_t>(from)];
        const Row& target = geometry_.rows[static_cast<std::size_t>(to)];
        const float x = RowOffsetX(source, static_cast<std::size_t>(caretCol_));
        caretLine_ = target.line;
        caretCol_ = static_cast<int>(RowColAt(target, x));
      }
    } else {
      caretLine_ = std::clamp(caretLine_ + dLine, 0, static_cast<int>(lines_.size()) - 1);
    }
    ClampCaret();
  }
  blinkAnchor_ = GetTime();
}

void CodeBlock::MoveCaretByWord(int direction) {
  const std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  const int size = static_cast<int>(line.size());

  if (direction < 0) {
    if (caretCol_ == 0) { MoveCaret(0, -1); return; }
    int i = caretCol_;
    // Step back over any run of spaces, then over the word before it.
    while (i > 0 && !IsWordChar(line[static_cast<std::size_t>(i - 1)])) --i;
    while (i > 0 && IsWordChar(line[static_cast<std::size_t>(i - 1)])) --i;
    caretCol_ = i;
  } else {
    if (caretCol_ >= size) { MoveCaret(0, +1); return; }
    int i = caretCol_;
    while (i < size && IsWordChar(line[static_cast<std::size_t>(i)])) ++i;
    while (i < size && !IsWordChar(line[static_cast<std::size_t>(i)])) ++i;
    caretCol_ = i;
  }
  blinkAnchor_ = GetTime();
}

void CodeBlock::HandleInput() {
  const bool ctrl = CtrlDown();

  if (ctrl && Triggered(KEY_Z)) {
    Undo();
    return;
  }

  for (int codepoint = GetCharPressed(); codepoint > 0; codepoint = GetCharPressed()) {
    if (codepoint >= 32 && !ctrl) InsertCodepoint(codepoint);
  }

  if (ctrl && Triggered(KEY_LEFT)) { MoveCaretByWord(-1); return; }
  if (ctrl && Triggered(KEY_RIGHT)) { MoveCaretByWord(+1); return; }

  // Ctrl+Enter is the compile shortcut, so it must not also insert a newline.
  if (Triggered(KEY_ENTER) && !ctrl) InsertNewline();
  if (Triggered(KEY_KP_ENTER) && !ctrl) InsertNewline();
  if (Triggered(KEY_BACKSPACE)) DeleteBefore();
  if (Triggered(KEY_DELETE)) DeleteAt();
  if (Triggered(KEY_LEFT)) MoveCaret(0, -1);
  if (Triggered(KEY_RIGHT)) MoveCaret(0, +1);
  if (Triggered(KEY_UP)) MoveCaret(-1, 0);
  if (Triggered(KEY_DOWN)) MoveCaret(+1, 0);
  if (Triggered(KEY_TAB)) {
    for (int i = 0; i < 4; ++i) InsertCodepoint(' ');
  }
  if (IsKeyPressed(KEY_HOME)) {
    caretCol_ = 0;
    blinkAnchor_ = GetTime();
  }
  if (IsKeyPressed(KEY_END)) {
    caretCol_ = static_cast<int>(lines_[static_cast<std::size_t>(caretLine_)].size());
    blinkAnchor_ = GetTime();
  }
}

bool CodeBlock::HandleChrome(const Viewport& viewport, FontStore& fonts) {
  // Asked of every box on the canvas now that a text box shows its controls
  // without the caret, so it has to say no before measuring anything: Measure
  // already runs several times a frame per box and this must not add another.
  if (!focused_ && !IsText() && !marginDrag_) return false;
  const Geometry& g = Measure(viewport, fonts);
  const Vector2   pointer = GetMousePosition();
  const bool      pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

  // Setting the measure, once it has begun, owns the pointer until it is let
  // go -- like every other drag on this canvas.
  if (marginDrag_) {
    const float zoom = std::max(g.zoom, 1e-6f);
    const float padX = kPadX * (em_ / kBaseEm) * zoom;
    const float wanted = (pointer.x + kRimGrabInside - g.rect.x - 2.0f * padX) / zoom;
    textWidth_ = std::clamp(wanted, em_ * 3.0f, em_ * 400.0f);
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) marginDrag_ = false;
    return true;
  }

  if (!ChromeVisible(g, pointer)) return false;
  const Chrome chrome = ChromeRects(g);

  if (chrome.forText) {
    if (CheckCollisionPointRec(pointer, MarginHandle(g))) {
      if (pressed) marginDrag_ = true;
      return true;
    }
    if (CheckCollisionPointRec(pointer, chrome.bold)) {
      if (pressed) bold_ = !bold_;
      return true;
    }
    if (CheckCollisionPointRec(pointer, chrome.italic)) {
      if (pressed) italic_ = !italic_;
      return true;
    }
    for (int i = 0; i < 3; ++i) {
      if (!CheckCollisionPointRec(pointer, chrome.justify[i])) continue;
      if (pressed) justify_ = static_cast<Justify>(i);
      return true;
    }
    if (CheckCollisionPointRec(pointer, chrome.toggle)) {
      if (pressed) showSource_ = !showSource_;
      return true;
    }
    return false;
  }

  if (!CheckCollisionPointRec(pointer, chrome.toggle)) return false;
  if (pressed) showMath_ = !showMath_;
  return true;   // claim the hover too, so a click never falls through
}

void CodeBlock::RequestCompile() {
  dirty_ = false;

  // A text box is not a program. Nothing about words compiles, and trying
  // would put a diagnostic on a sentence.
  if (IsText()) {
    program_.Rebuild({});
    outcome_ = {};
    return;
  }

  // The mathematical reading comes first. Only a box that declares nothing
  // falls back to the older `bool testFunc()` behaviour.
  program_.Rebuild(lines_);
  if (program_.state() != mathprogram::State::Empty) {
    outcome_ = {};
    return;
  }

  if (pending_.valid()) return;
  outcome_.status = compiler::Status::Working;
  pending_ = std::async(std::launch::async,
                        [source = Text()] { return compiler::BuildAndRun(source); });
}

void CodeBlock::ReleaseGpu() {
  if (haveShader_) {
    UnloadShader(shader_);
    haveShader_ = false;
  }
  if (haveProbe_) {
    UnloadShader(probeShader_);
    haveProbe_ = false;
  }
  if (haveProbeTarget_) {
    UnloadRenderTexture(probeTarget_);
    haveProbeTarget_ = false;
  }
  if (havePreviewSurface_) {
    UnloadTexture(previewSurface_);
    havePreviewSurface_ = false;
  }
  uniforms_.clear();
  probeUniforms_.clear();
  probeSignature_.clear();
}

void CodeBlock::ProbeIfNeeded() {
  if (!haveProbe_ || !haveProbeTarget_ || !havePreviewSurface_) return;

  // Nothing has moved, so the answer is the one already on screen.
  std::vector<float> signature;
  signature.reserve(probeUniforms_.size() + 2);
  signature.push_back(probePoint_.x);
  signature.push_back(probePoint_.y);
  for (const auto& [name, location] : probeUniforms_) {
    const mathprogram::Variable* variable = FindVariable(name);
    signature.push_back(variable == nullptr ? 0.0f : variable->value);
  }
  if (signature == probeSignature_) return;
  probeSignature_ = signature;

  for (const auto& [name, location] : probeUniforms_) {
    if (location < 0) continue;
    const mathprogram::Variable* variable = FindVariable(name);
    if (variable == nullptr) continue;
    const float value = variable->value;
    SetShaderValue(probeShader_, location, &value, SHADER_UNIFORM_FLOAT);
  }
  if (probePointLocation_ >= 0) {
    const float point[2] = {probePoint_.x, probePoint_.y};
    SetShaderValue(probeShader_, probePointLocation_, point, SHADER_UNIFORM_VEC2);
  }

  const float width = static_cast<float>(probeTarget_.texture.width);
  BeginTextureMode(probeTarget_);
  // The pixels are data, not a picture: blending them with what was there
  // before would corrupt every byte.
  rlDisableColorBlend();
  BeginShaderMode(probeShader_);
  DrawTexturePro(previewSurface_, {0.0f, 0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, width, 1.0f},
                 {0.0f, 0.0f}, 0.0f, WHITE);
  EndShaderMode();
  rlEnableColorBlend();
  EndTextureMode();

  const Image pixels = LoadImageFromTexture(probeTarget_.texture);
  if (pixels.data != nullptr) {
    const unsigned char* bytes = static_cast<const unsigned char*>(pixels.data);
    const std::vector<std::string>& names = program_.probedNames();
    std::vector<float> values;
    values.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
      const std::uint32_t bits = (static_cast<std::uint32_t>(bytes[i * 4 + 0]) << 24) |
                                 (static_cast<std::uint32_t>(bytes[i * 4 + 1]) << 16) |
                                 (static_cast<std::uint32_t>(bytes[i * 4 + 2]) << 8) |
                                 static_cast<std::uint32_t>(bytes[i * 4 + 3]);
      float value = 0.0f;
      std::memcpy(&value, &bits, sizeof(value));
      values.push_back(value);
    }
    program_.SetProbedValues(values);
  }
  UnloadImage(pixels);
}

void CodeBlock::CompileShader() {
  if (haveShader_) {
    UnloadShader(shader_);
    haveShader_ = false;
  }
  uniforms_.clear();

  std::string log;
  const bool ok = CompileFragmentShader(program_.glslSource(), &shader_, &log);
  haveShader_ = ok;

  if (ok) {
    // The knobs drive uniforms. Their slots are looked up once here rather than
    // every frame: each one is a round trip to the driver.
    for (const mathprogram::Variable& variable : program_.variables()) {
      if (!variable.free || variable.kind != mathprogram::Kind::Real) continue;
      uniforms_.emplace_back(variable.name,
                             GetShaderLocation(shader_, variable.name.c_str()));
    }
    if (!havePreviewSurface_) {
      // Something for the shader to cover. One white pixel stretched over the
      // preview is what makes fragTexCoord run 0..1 across it.
      Image white = GenImageColor(1, 1, WHITE);
      previewSurface_ = LoadTextureFromImage(white);
      UnloadImage(white);
      havePreviewSurface_ = true;
    }

    // And the shader that answers rather than draws, with one pixel of target
    // for each derived variable it has to report.
    const std::vector<std::string>& probed = program_.probedNames();
    if (!probed.empty()) {
      std::string probeLog;
      haveProbe_ = CompileFragmentShader(program_.glslProbeSource(), &probeShader_, &probeLog);
      if (haveProbe_) {
        for (const mathprogram::Variable& variable : program_.variables()) {
          if (!variable.free || variable.kind != mathprogram::Kind::Real) continue;
          probeUniforms_.emplace_back(variable.name,
                                      GetShaderLocation(probeShader_, variable.name.c_str()));
        }
        probePointLocation_ = GetShaderLocation(probeShader_, "sbProbe");
        probeTarget_ = LoadRenderTexture(static_cast<int>(probed.size()), 1);
        haveProbeTarget_ = true;
        probeSignature_.clear();
      }
    }
  }

  program_.ShaderCompiled(ok, log);
}

void CodeBlock::Poll() {
  program_.Poll();

  // The C++ compile runs on a worker thread, but a shader has to be compiled on
  // the thread holding the graphics context. So this half happens here.
  const mathprogram::State state = program_.state();
  if (state == mathprogram::State::Shading) CompileShader();

  // A box edited back into valid C++ should stop drawing the picture it used to
  // be. While a rebuild is in flight the old one stays up, the way the knobs
  // keep their old values.
  if (haveShader_ && state != mathprogram::State::ShaderReady &&
      state != mathprogram::State::Building && state != mathprogram::State::Shading) {
    ReleaseGpu();
  }

  // Ask the shader what its derived variables came to, so the number lines have
  // something to show. The point they are read at was settled while drawing the
  // last frame, which is close enough to be imperceptible.
  ProbeIfNeeded();

  if (!pending_.valid()) return;
  if (pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  outcome_ = pending_.get();
  pending_ = {};
}

void CodeBlock::Draw(const Viewport& viewport, FontStore& fonts) const {
  // Hover is resolved here so the next Measure sees it; a line under the
  // pointer shows its code instead of its LaTeX.
  hoverLine_ = -1;
  const Geometry& probe = Measure(viewport, fonts);
  if (probe.emPx < 0.4f) return;
  const Vector2 pointer = GetMousePosition();
  rimHot_ = PointOnRim(probe.rect, pointer);
  // Reaching for the rim should not flip the line underneath it to code.
  if (!rimHot_ && CheckCollisionPointRec(pointer, probe.rect)) {
    const int row = RowAt(pointer, viewport, fonts);
    if (row >= 0 && row < static_cast<int>(probe.rows.size())) {
      hoverLine_ = probe.rows[static_cast<std::size_t>(row)].line;
    }
  }

  const Geometry& g = Measure(viewport, fonts);
  // Absorbed by the drop target: the box is not on the canvas at the moment,
  // and letting go here is what throws it away.
  if (absorbed_) return;

  // A text box wears no card: prose on a slide is ink on the canvas, not a
  // window sitting on it -- and it keeps that look while it is being written
  // in, because what you are typing is what it is going to be. The dark card
  // comes back only when the source is what is showing, since then it really is
  // code.
  const bool prose = IsText();
  const bool asWords = prose && !showSource_;
  if (!asWords) {
    const Color border = focused_ ? kAccent : Fade(kCodeFg, 0.22f);
    DrawCard(g.rect, g.zoom, kCodeBg, border, focused_ ? 2.0f : 1.0f);
  }
  const Color ink = asWords ? kSlideInk : kCodeFg;

  for (std::size_t r = 0; r < g.rows.size(); ++r) {
    const Row&        row = g.rows[r];
    const std::size_t i = static_cast<std::size_t>(row.line);

    // A window being used elsewhere lights up every place its variable is
    // written, in whichever reading this row happens to be showing.
    for (const std::string& name : highlights_) {
      const Color tint = VariableColor(name);
      for (const Rectangle& box : MentionRects(r, name, g, fonts)) {
        DrawRectangleRounded(box, 0.30f, 6, Fade(tint, kMentionFillAlpha));
        DrawRectangleRoundedLinesEx(box, 0.30f, 6, std::max(1.0f, 1.1f * g.zoom), tint);
      }
    }

    if (row.asLatex) {
      const tex::Layout& layout =
          prose ? cache_[i].pieceLayout[static_cast<std::size_t>(row.piece)] : cache_[i].layout;

      tex::DrawLayout(layout, fonts, {row.x, row.baseline}, ink);
      continue;
    }

    const std::string& source = lines_[i];
    const std::vector<highlight::Span> spans = highlight::Cpp(source);

    // The injected initialiser, shown while previewing but never while editing:
    // it is generated, not yours to change. It slots in *before* the semicolon,
    // so the line is drawn in three pieces.
    const std::string injection = focused_ ? std::string() : Injection(static_cast<int>(i));

    if (injection.empty()) {
      DrawSpans(fonts, source, spans, 0, source.size(), {row.x, row.baseline}, g.emPx);
      continue;
    }

    const std::size_t semicolon = source.rfind(';');
    const std::size_t split = (semicolon == std::string::npos) ? source.size() : semicolon;

    float x = row.x;
    x += DrawSpans(fonts, source, spans, 0, split, {x, row.baseline}, g.emPx);
    text::Draw(fonts, Face::Mono, injection, {x, row.baseline}, g.emPx, kInjected);
    x += text::Measure(fonts, Face::Mono, injection, g.emPx);
    DrawSpans(fonts, source, spans, split, source.size(), {x, row.baseline}, g.emPx);
  }

  // Caret. Only while this box is actually being edited, and solid while
  // typing so it never distracts mid-edit.
  const int caretRow = focused_ ? CaretRow(g) : -1;
  if (caretRow >= 0) {
    const double phase = std::fmod(GetTime() - blinkAnchor_, 1.06);
    if (phase < 0.62) {
      const Row&         row = g.rows[static_cast<std::size_t>(caretRow)];
      const std::string& line = lines_[static_cast<std::size_t>(row.line)];
      const std::size_t  col =
          std::clamp(static_cast<std::size_t>(caretCol_), row.begin, row.end);
      const float x =
          row.x + text::Measure(fonts, row.asLatex ? ProseFace() : Face::Mono,
                                std::string_view(line).substr(row.begin, col - row.begin),
                                g.emPx);
      DrawRectangleRec({x, row.y + row.height * 0.08f, std::max(1.0f, g.emPx * 0.07f),
                        row.height * 0.86f},
                       asWords ? kSlideInk : kCaret);
    }
  }

  // Chrome: the box with the caret, or a text box being looked at.
  if (ChromeVisible(g, pointer)) {
    const Chrome chrome = ChromeRects(g);
    const float  em = kChromeEm * g.zoom;

    if (chrome.forText) {
      // Each control shows its own effect: the B is bold, the I is italic, and
      // the justification buttons are ragged on the side they leave ragged.
      const auto button = [&](Rectangle rect, bool on) {
        const bool hot = CheckCollisionPointRec(pointer, rect);
        DrawRectangleRounded(rect, 0.32f, 8, on ? kAccent : (hot ? kChromeHover : kChromeBg));
      };
      const auto letter = [&](Rectangle rect, Face face, const char* glyph) {
        if (em < 3.0f) return;
        const float width = text::Measure(fonts, face, glyph, em);
        text::Draw(fonts, face, glyph,
                   {rect.x + (rect.width - width) * 0.5f, rect.y + rect.height * 0.5f + em * 0.34f},
                   em, kChromeText);
      };

      button(chrome.bold, bold_);
      letter(chrome.bold, Face::MainBold, "B");
      button(chrome.italic, italic_);
      letter(chrome.italic, Face::MainItalic, "I");

      // Same control a code box has, in the same place, doing the same job.
      {
        const bool overToggle = CheckCollisionPointRec(pointer, chrome.toggle);
        DrawRectangleRounded(chrome.toggle, 0.35f, 8,
                             showSource_ ? kAccent : (overToggle ? kChromeHover : kChromeBg));
        const char* label = showSource_ ? "text" : "code";
        const float labelWidth = text::Measure(fonts, Face::MainRegular, label, em);
        if (em >= 3.0f) {
          text::Draw(fonts, Face::MainRegular, label,
                     {chrome.toggle.x + (chrome.toggle.width - labelWidth) * 0.5f,
                      chrome.toggle.y + chrome.toggle.height * 0.5f + em * 0.34f},
                     em, kChromeText);
        }
      }

      // The measure, as a bar sitting on the right margin it sets.
      const Rectangle margin = MarginHandle(g);
      const bool      overMargin = marginDrag_ || CheckCollisionPointRec(pointer, margin);
      DrawRectangleRec({margin.x + margin.width - kMarginBar, margin.y, kMarginBar, margin.height},
                       overMargin ? kAccent : Fade(kSlideInk, 0.35f));

      for (int i = 0; i < 3; ++i) {
        const Rectangle rect = chrome.justify[i];
        button(rect, justify_ == static_cast<Justify>(i));
        const float pad = rect.width * 0.22f;
        const float full = rect.width - 2.0f * pad;
        const float thickness = std::max(1.0f, rect.height * 0.075f);
        const float gap = (rect.height - 2.0f * pad) / 3.0f;
        for (int line = 0; line < 4; ++line) {
          // Alternate lines are short, which is what makes one side look ragged
          // and the other flush.
          const float width = (line % 2 == 0) ? full : full * 0.62f;
          float x = rect.x + pad;
          if (i == 1) x = rect.x + (rect.width - width) * 0.5f;
          if (i == 2) x = rect.x + rect.width - pad - width;
          DrawRectangleRec({x, rect.y + pad + gap * static_cast<float>(line), width, thickness},
                           kChromeText);
        }
      }
    } else if (focused_) {
      const bool overToggle = CheckCollisionPointRec(pointer, chrome.toggle);
      DrawRectangleRounded(chrome.toggle, 0.35f, 8, overToggle ? kChromeHover : kChromeBg);
      // The label names what you will get, not what you have.
      const char* label = showMath_ ? "code" : "math";
      const float labelWidth = text::Measure(fonts, Face::MainRegular, label, em);
      text::Draw(fonts, Face::MainRegular, label,
                 {chrome.toggle.x + (chrome.toggle.width - labelWidth) * 0.5f,
                  chrome.toggle.y + chrome.toggle.height * 0.5f + em * 0.34f},
                 em, kChromeText);
    }
  }

  // A text box being written in has no card to say so, so its rim does.
  if (rimHot_ || dragging_ || selected_ || (asWords && focused_)) {
    DrawRim(g.rect, g.zoom, dragging_ || selected_ || focused_);
  }

  // The legacy result square, only for a box that has no variables of its own.
  if (outcome_.status != compiler::Status::NotRun) {
    const float side = kSquareSide * g.zoom;
    const Rectangle square = {g.rect.x + g.rect.width + kSquareGap * g.zoom,
                              g.rect.y + (g.rect.height - side) * 0.5f, side, side};
    switch (outcome_.status) {
      case compiler::Status::True:
        DrawCard(square, g.zoom, kResultTrue, Fade(BLACK, 0.18f), 1.0f);
        break;
      case compiler::Status::False:
        DrawCard(square, g.zoom, kResultFalse, Fade(BLACK, 0.18f), 1.0f);
        break;
      default:
        DrawCard(square, g.zoom, kResultUnknown, Fade(BLACK, 0.15f), 1.0f);
        break;
    }
  }
}

void CodeBlock::DrawOverlay(const Viewport& viewport, FontStore& fonts) const {
  // A line from the pointer to every place the thing under it is written. The
  // box round the name says *that* it is the same thing; the curve says which
  // pointer it is the same thing as, which matters once two windows are open.
  //
  // Drawn in the overlay pass so it runs over every window, including the one
  // being pointed at -- a curve chopped in half by a card would say nothing.
  if (!highlights_.empty() && !absorbed_) {
    const Geometry& g = Measure(viewport, fonts);
    if (g.emPx >= 0.4f) {
      const Vector2 pointer = GetMousePosition();
      const float thickness = std::max(1.4f, 1.8f * g.zoom);
      for (const std::string& name : highlights_) {
        const Color tint = VariableColor(name);
        for (std::size_t i = 0; i < g.rows.size(); ++i) {
          for (const Rectangle& box : MentionRects(i, name, g, fonts)) {
            // It lands on whichever side the pointer is already on, so the
            // curve never crosses the name it is pointing at.
            const bool fromLeft = pointer.x < box.x + box.width * 0.5f;
            const Vector2 target = {fromLeft ? box.x : box.x + box.width,
                                    box.y + box.height * 0.5f};
            // Tangents leave both ends horizontally, which is what makes it
            // read as a cable rather than as a stray line.
            const float reach =
                std::clamp(std::abs(target.x - pointer.x) * 0.45f, 24.0f, 190.0f);
            const float direction = fromLeft ? 1.0f : -1.0f;
            DrawSplineSegmentBezierCubic(pointer, {pointer.x + reach * direction, pointer.y},
                                         {target.x - reach * direction, target.y}, target,
                                         thickness, Fade(tint, 0.8f));
          }
        }
      }
    }
  }

  // Whichever half of the machinery reported a problem, it is shown the same
  // way: against the character the compiler pointed at.
  bool        failed = false;
  int         errorLine = 1;
  int         errorColumn = 1;
  std::string message;

  if (program_.state() == mathprogram::State::Failed && program_.error().present) {
    failed = true;
    errorLine = program_.error().line;
    errorColumn = program_.error().column;
    message = program_.error().message;
  } else if (program_.state() == mathprogram::State::NoCompiler) {
    message = program_.detail();
  } else if (outcome_.status == compiler::Status::Failed && outcome_.error.present) {
    failed = true;
    errorLine = outcome_.error.line;
    errorColumn = outcome_.error.column;
    message = outcome_.error.message;
  } else if (outcome_.status == compiler::Status::NoCompiler ||
             outcome_.status == compiler::Status::Crashed) {
    message = outcome_.detail.empty() ? "the program did not run" : outcome_.detail;
  }
  if (message.empty()) return;

  const Geometry& g = Measure(viewport, fonts);
  if (g.emPx < 0.4f || g.rows.empty()) return;

  float anchorX = g.textLeftTop.x;
  float anchorBottom = g.rows.front().y + g.rows.front().height;

  if (failed) {
    const int lineCount = static_cast<int>(lines_.size());
    const int line = std::clamp(errorLine, 1, lineCount) - 1;
    const std::string& text = lines_[static_cast<std::size_t>(line)];
    const int column = std::clamp(errorColumn, 1, static_cast<int>(text.size()) + 1) - 1;

    anchorX = g.textLeftTop.x +
              text::Measure(fonts, Face::Mono,
                            std::string_view(text).substr(0, static_cast<std::size_t>(column)),
                            g.emPx);
    const Row& row = g.rows[static_cast<std::size_t>(line)];
    anchorBottom = row.y + row.height;

    // A red caret under the offending character, the way the compiler draws it.
    text::Draw(fonts, Face::Mono, "^", {anchorX, anchorBottom + g.ascent * 0.16f}, g.emPx,
               kErrorCaret);
    anchorBottom += g.lineHeight * 0.30f;
  }

  // Deliberately not monospaced, at 75% of the code size, black on white.
  const float messageEm = g.emPx * kErrorEmScale;
  const float maxWidth = kErrorMaxWidth * g.zoom;
  const std::vector<std::string> wrapped =
      text::Wrap(fonts, Face::MainRegular, message, messageEm, maxWidth);
  if (wrapped.empty()) return;

  const float lineHeight = text::LineHeight(fonts, Face::MainRegular, messageEm);
  const float ascent = text::Ascent(fonts, Face::MainRegular, messageEm);
  const float pad = kErrorPad * g.zoom;

  float width = 0.0f;
  for (const std::string& line : wrapped) {
    width = std::max(width, text::Measure(fonts, Face::MainRegular, line, messageEm));
  }

  // Top-left corner just below the character that the error points at.
  const Rectangle box = {anchorX, anchorBottom + 2.0f * g.zoom, width + 2.0f * pad,
                         lineHeight * static_cast<float>(wrapped.size()) + 2.0f * pad};
  DrawCard(box, g.zoom, kErrorBg, kErrorCaret, 1.0f);

  float baseline = box.y + pad + ascent;
  for (const std::string& line : wrapped) {
    text::Draw(fonts, Face::MainRegular, line, {box.x + pad, baseline}, messageEm, kErrorFg);
    baseline += lineHeight;
  }
}

// ------------------------------------------------------------------- maths
MathBlock::MathBlock(Vector2 worldTopLeft, float worldEm, std::string latex, bool displayStyle)
    : origin_(worldTopLeft),
      em_(worldEm),
      source_(std::move(latex)),
      display_(displayStyle),
      parsed_(tex::Parse(source_)) {}

void MathBlock::Draw(const Viewport& viewport, FontStore& fonts) const {
  const float zoom = viewport.Zoom();
  const float emPx = em_ * zoom;
  if (emPx < 0.4f) return;

  const tex::Layout layout = tex::BuildLayout(parsed_, fonts, emPx, display_);

  const Vector2 topLeft = viewport.ToScreen(origin_);
  const Rectangle rect = {topLeft.x, topLeft.y, layout.width + 2.0f * kPadX * zoom,
                          layout.height + layout.depth + 2.0f * kPadY * zoom};

  DrawCard(rect, zoom, kMathBg, Fade(kMathFg, 0.15f), 1.0f);

  const Vector2 origin = {rect.x + kPadX * zoom, rect.y + kPadY * zoom + layout.height};
  tex::DrawLayout(layout, fonts, origin, kMathFg);
}

}  // namespace sb
