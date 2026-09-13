#include "blocks.h"

#include "logging.h"

#include <limits>

#include "glext.h"

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
#include "hover.h"
#include "theme.h"
#include "grid.h"
#include "types/examples.h"

namespace sb {
namespace {

// Card geometry, in world units so it scales with the canvas. The padding is one
// grid step, the same on every side: a card snapped by its corner then has its
// text on a dot as well.
float CardPad() { return GridBaseStep(); }
constexpr float kRadius = 8.0f;
// The type size those numbers were chosen against. Padding is kept in
// proportion to it so that resizing a box is genuinely uniform: at the default
// size nothing has moved, and at any other size the whole card has scaled.
constexpr float kBaseEm = 26.0f;

// An empty box still needs to be big enough to see and click.
constexpr int kMinColumns = 14;

// The result square, to the right of the card.
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

// Holding shift while the caret moves leaves the anchor where it was, which is
// what turns a movement into a highlight. One rule for every way of moving.
bool ShiftDown() { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }

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

// A null half means raylib's own, which is what supplies fragTexCoord and
// fragColor one way and the screen-space mvp the other.
bool CompileShaderHalf(const char* vertex, const char* fragment, Shader* shader,
                       std::string* log) {
  log->clear();
  g_shaderLog = log;
  SetTraceLogCallback(CaptureTraceLog);
  const Shader compiled = LoadShaderFromMemory(vertex, fragment);
  // Back to the filter rather than to nothing: putting nothing back would let
  // raylib narrate freely again from the next texture onwards.
  SetTraceLogCallback(log::Filtered);
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
  if (haveShader_ || haveFnTarget_ || havePreviewSurface_ || haveProbeTarget_ ||
      haveVizShader_ || samplerProgram_ != 0 || samplerBuffer_ != 0) {
    TraceLog(LOG_WARNING, "BLOCKS: a box was destroyed still holding GPU memory");
  }
}

clifford::Signature CodeBlock::Algebra() const {
  clifford::Signature signature = clifford::kDefaultSignature;
  for (const std::string& line : lines_) clifford::ParseAlgebra(line, &signature);
  return signature;
}

void CodeBlock::EnsureCache(FontStore& fonts, float emPx, float measurePx) const {
  const bool     prose = IsText();
  const unsigned styleBits = (bold_ ? 1u : 0u) | (italic_ ? 2u : 0u) | (prose ? 4u : 0u);

  // The cheapest question first, because it is nearly always the answer: has
  // anything moved at all? The algebra is deliberately not asked for here. It
  // is read off the lines themselves, so no line changing means it cannot have
  // changed either -- and asking costs a parse of every line.
  bool stale = cache_.size() != lines_.size() || cacheStyle_ != styleBits ||
               cacheEmPx_ != emPx || (prose && cacheWrap_ != measurePx);
  for (std::size_t i = 0; !stale && i < lines_.size(); ++i) {
    stale = cache_[i].source != lines_[i];
  }
  if (!stale) return;

  cacheStyle_ = styleBits;
  cacheEmPx_ = emPx;
  cacheWrap_ = measurePx;
  ++cacheVersion_;

  // Bold and italic change which face the words are set in, so they invalidate
  // a line as surely as editing it does -- and so does the algebra, which every
  // line of a box is read in terms of. Changing it on one line has to re-read
  // all of them.
  const clifford::Signature signature = Algebra();
  const unsigned styleSig = styleBits | (static_cast<unsigned>(signature.p) << 3) |
                            (static_cast<unsigned>(signature.q) << 8) |
                            (static_cast<unsigned>(signature.r) << 13);

  // Which function each line is inside. A `return` is not a statement about
  // anything until you know what it is the answer of, and a line on its own
  // does not say -- the lines around it do, and this is the only place that can
  // see them.
  std::vector<mathline::Inside> inside(lines_.size());
  if (!prose) {
    for (const mathline::FunctionSpan& span : mathline::FindFunctions(lines_, signature)) {
      for (int k = span.first + 1; k <= span.last; ++k) {
        inside[static_cast<std::size_t>(k)] = {span.head.name, span.head.paramName,
                                               k == span.last};
      }
    }
  }

  cache_.resize(lines_.size());
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    LineCache& entry = cache_[i];
    if (entry.source != lines_[i] || entry.styleSig != styleSig || entry.inside != inside[i]) {
      entry.source = lines_[i];
      entry.styleSig = styleSig;
      entry.inside = inside[i];
      // A text box has no mathematical reading and is not looked for one: its
      // lines are words, and words are not code that failed to parse.
      entry.parsed =
          prose ? mathline::Line{} : mathline::Parse(lines_[i], signature, inside[i]);
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
  const bool failed = program_.state() == mathprogram::State::Failed;
  return (failed && program_.error().present) ? program_.error().line : 0;
}

bool CodeBlock::ReadsAsCode() const {
  if (reading_ == Reading::Code) return true;
  if (reading_ == Reading::Maths) return false;
  // Auto. Editing a box means editing all of it -- and a box that says how
  // something is *drawn* is code all the way through even when nobody is
  // editing it, since there is no mathematical reading of a mesh.
  return focused_ || !program_.vizType().empty();
}

bool CodeBlock::ShowsCode(int line) const {
  if (line < 0 || line >= static_cast<int>(cache_.size())) return true;
  // A text box is words all the way through, and stays words while it is being
  // edited: what you are writing is what you can see. The one way to get at the
  // source -- and so at the sigil, which is how a text box stops being one --
  // is the toggle on its own strip.
  if (IsText()) return showSource_;
  if (ReadsAsCode()) return true;
  if (cache_[static_cast<std::size_t>(line)].latex.empty()) return true;
  // A line the compiler complained about shows its code, so the red caret has
  // real characters to point at -- and so you can see what to fix.
  if (line + 1 == ErrorLine()) return true;
  if (focused_ && line == caretLine_) return true;
  if (line == hoverLine_) return true;
  return false;
}

std::string CodeBlock::Injection(int line) const {
  if (line < 0 || line >= static_cast<int>(cache_.size())) return {};
  const mathline::Line& parsed = cache_[static_cast<std::size_t>(line)].parsed;

  // A function nobody gave a body has one chosen for it, and the choice is a
  // piece of code rather than a number: it takes the place of the semicolon.
  // Not if the box defines the function further down, since then the body is
  // the user's own and this line is only saying it is coming.
  if (parsed.kind == mathline::Kind::Prototype) {
    mathline::FunctionHead head;
    if (!mathline::ParseFunctionHead(lines_[static_cast<std::size_t>(line)], &head, Algebra())) {
      return {};
    }
    for (const mathprogram::Function& fn : program_.functions()) {
      if (fn.name != head.name || fn.firstLine != line) continue;
      return " { return " +
             types::realfunction::ExampleFunctionBody(head.returnType, head.paramType,
                                                      head.paramName, head.name, Algebra()) +
             "; }";
    }
    return {};
  }
  if (parsed.kind != mathline::Kind::Declaration || parsed.hasInitialiser) return {};
  // A multivector gets no injection shown at all. Not because there is nothing
  // to show -- there is, and it is real code the way a function's is -- but
  // because a box is sized for the worst case across both readings, and a
  // multivector of Cl(3,0,1) has sixteen components. Showing it makes the box
  // wider than the screen even while it is hidden. Better to show nothing than
  // to show a nought that is not the value, and better than either would be a
  // window that displays one; that is the next thing.
  if (mathline::IsMultivectorType(parsed.type, Algebra())) return {};

  // The literal this line is given, which is what the knob chose -- not what the
  // variable has become by wherever the box is being read, since a line further
  // down may have assigned over it.
  for (const mathprogram::Variable& variable : program_.variables()) {
    if (variable.name != parsed.name) continue;
    const int governing = variable.GoverningLiteral(line);
    if (governing >= 0) {
      return " = " + FormatInjected(variable.literals[static_cast<std::size_t>(governing)].value);
    }
    return " = " + FormatInjected(variable.value);
  }
  return " = " + FormatInjected(types::ExampleReal(parsed.name));
}

std::vector<Rectangle> CodeBlock::MentionsOf(const std::string& name, const Viewport& viewport,
                                             FontStore& fonts) const {
  std::vector<Rectangle> found;
  if (absorbed_) return found;
  const Geometry& g = Measure(viewport, fonts);
  if (g.emPx < 0.4f) return found;
  for (std::size_t r = 0; r < g.rows.size(); ++r) {
    for (const Rectangle& box : MentionRects(r, name, g, fonts)) found.push_back(box);
  }
  return found;
}

std::string CodeBlock::MentionAt(Vector2 screenPoint, const Viewport& viewport,
                                 FontStore& fonts) const {
  if (absorbed_ || IsText()) return {};
  const Geometry& g = Measure(viewport, fonts);
  if (g.emPx < 0.4f || !CheckCollisionPointRec(screenPoint, g.rect)) return {};
  const int row = RowAt(screenPoint, viewport, fonts);
  if (row < 0) return {};
  for (const mathprogram::Variable& variable : program_.variables()) {
    for (const Rectangle& box : MentionRects(static_cast<std::size_t>(row), variable.name, g, fonts)) {
      if (CheckCollisionPointRec(screenPoint, box)) return variable.name;
    }
  }
  return {};
}

mathline::Flow CodeBlock::FlowFor(const std::string& name) const {
  mathline::Flow            latest;
  const clifford::Signature sig = Algebra();
  for (const std::string& raw : lines_) {
    const mathline::Line parsed = mathline::Parse(raw, sig);
    if (parsed.name != name || !parsed.hasInitialiser) continue;
    if (parsed.kind != mathline::Kind::Declaration && parsed.kind != mathline::Kind::Assignment) {
      continue;
    }
    mathline::Flow flow = mathline::Wiring(raw, sig);
    // The *last* line that defines it wins, since a variable is mutable and
    // what its window shows is what the block ended with.
    if (flow.Interesting()) latest = std::move(flow);
  }
  return latest;
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

void CodeBlock::SetHighlights(std::vector<ActiveValue> values) {
  highlights_ = std::move(values);
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

// Whether `name` is written, as a whole word, anywhere in the stretch of the
// box between two positions. Touching a mention is enough -- highlighting one
// letter of a name is how you take hold of what that name refers to, so it had
// better be how you ask about it too.
bool CodeBlock::MentionedBetween(const std::string& name, int fromLine, int fromCol, int toLine,
                                 int toCol, bool wholeLines) const {
  if (name.empty()) return false;
  for (int i = std::max(fromLine, 0); i <= toLine && i < static_cast<int>(lines_.size()); ++i) {
    const std::string& line = lines_[static_cast<std::size_t>(i)];
    std::size_t begin = 0;
    std::size_t end = line.size();
    if (!wholeLines) {
      if (i == fromLine) begin = std::min(static_cast<std::size_t>(fromCol), line.size());
      if (i == toLine) end = std::min(static_cast<std::size_t>(toCol), line.size());
    }

    for (std::size_t at = line.find(name); at != std::string::npos;
         at = line.find(name, at + 1)) {
      const std::size_t after = at + name.size();
      const bool clearBefore = at == 0 || !IsWordChar(line[at - 1]);
      const bool clearAfter = after >= line.size() || !IsWordChar(line[after]);
      if (!clearBefore || !clearAfter) continue;
      if (at < end && after > begin) return true;
    }
  }
  return false;
}

std::vector<std::string> CodeBlock::HighlightedVariables() const {
  std::vector<std::string> named;
  if (!focused_ || !HasSelection()) return named;

  int fromLine = 0;
  int fromCol = 0;
  int toLine = 0;
  int toCol = 0;
  SelectionRange(&fromLine, &fromCol, &toLine, &toCol);
  for (const mathprogram::Variable& variable : program_.variables()) {
    if (MentionedBetween(variable.name, fromLine, fromCol, toLine, toCol, /*wholeLines=*/false)) {
      named.push_back(variable.name);
    }
  }
  return named;
}

bool CodeBlock::InScope(const std::string& name) const {
  if (name.empty()) return true;

  // Before its declaration a variable does not exist, so there is nothing to
  // show: a window for it would be reporting a value it does not have yet.
  // This holds however the box is being read -- by the caret or by the pointer
  // -- because it is a fact about the program rather than about the reading.
  const int at = ExecutedUpTo();
  if (at >= 0) {
    const mathprogram::Variable* variable = FindVariable(name);
    if (variable != nullptr && variable->declaredLine > at) return false;
  }

  // Nobody is reading this box a line at a time, so all of it counts.
  if (!focused_) return true;

  int fromLine = caretLine_;
  int fromCol = caretCol_;
  int toLine = caretLine_;
  int toCol = caretCol_;
  // A bare caret is asking about its whole line; a highlight about exactly what
  // it covers.
  const bool selected = HasSelection();
  if (selected) SelectionRange(&fromLine, &fromCol, &toLine, &toCol);
  if (MentionedBetween(name, fromLine, fromCol, toLine, toCol, /*wholeLines=*/!selected)) {
    return true;
  }

  // And whatever a highlighted value is *made of* is part of what you are
  // looking at. Highlighting `d` in `d = a + b * c` draws the machine that
  // makes it, and a machine with its inputs' windows stood down would be a
  // diagram of nothing. Only asked when something is highlighted, which is
  // rare, since it means reading the box's lines again.
  if (!selected) return false;
  for (const mathprogram::Variable& variable : program_.variables()) {
    if (variable.name == name) continue;
    if (!MentionedBetween(variable.name, fromLine, fromCol, toLine, toCol,
                          /*wholeLines=*/false)) {
      continue;
    }
    const mathline::Flow flow = FlowFor(variable.name);
    for (const mathline::FlowNode& node : flow.nodes) {
      if (node.op.empty() && node.name == name) return true;
    }
  }
  return false;
}

void CodeBlock::SetInsertCaret(bool value) {
  // A caret offering something should be visible the moment it is offered,
  // rather than halfway through a blink.
  if (value && !insertCaret_) blinkAnchor_ = GetTime();
  insertCaret_ = value;
}

int CodeBlock::InsertLine() const {
  if (lines_.empty()) return 0;
  // Nowhere has been chosen yet, so the end of what is written is where the
  // next thing goes -- which is where a person would put it.
  if (!caretUsed_) return static_cast<int>(lines_.size()) - 1;
  return std::clamp(caretLine_, 0, static_cast<int>(lines_.size()) - 1);
}

std::string CodeBlock::FreshName() const {
  const auto spokenFor = [this](const std::string& name) {
    for (const mathprogram::Variable& variable : program_.variables()) {
      if (variable.name == name) return true;
    }
    // Not only the declared ones: a name written anywhere in the box is taken,
    // whether or not the build that would have told us so has finished.
    for (const std::string& line : lines_) {
      for (std::size_t at = line.find(name); at != std::string::npos;
           at = line.find(name, at + 1)) {
        const bool clearBefore = at == 0 || !IsWordChar(line[at - 1]);
        const std::size_t after = at + name.size();
        const bool clearAfter = after >= line.size() || !IsWordChar(line[after]);
        if (clearBefore && clearAfter) return true;
      }
    }
    return false;
  };

  // Single letters first, the way one names a value while thinking, and only
  // then letters with a number after them.
  for (int round = 0; round < 100; ++round) {
    for (char letter = 'a'; letter <= 'z'; ++letter) {
      std::string candidate(1, letter);
      if (round > 0) candidate += std::to_string(round + 1);
      if (!spokenFor(candidate)) return candidate;
    }
  }
  return "v";
}

void CodeBlock::InsertDeclaration(const std::string& declaration) {
  PushUndo(EditKind::Structural);
  if (lines_.empty()) lines_.emplace_back();

  int at = InsertLine();
  // A blank line is somewhere to write, not something to push out of the way.
  const std::string& target = lines_[static_cast<std::size_t>(at)];
  if (target.find_first_not_of(" 	") == std::string::npos) {
    lines_[static_cast<std::size_t>(at)] = declaration;
  } else {
    lines_.insert(lines_.begin() + at + 1, declaration);
    ++at;
  }

  caretLine_ = at;
  caretCol_ = static_cast<int>(lines_[static_cast<std::size_t>(at)].size());
  TouchEdit();
  RequestCompile();
}

int CodeBlock::VizUniform(const char* name) const {
  return haveVizShader_ ? GetShaderLocation(vizShader_, name) : -1;
}

// Which function of a given kind this box is being read inside, or the first
// one of that kind it defines. The same rule every other window follows about
// where the box has got to.
int CodeBlock::FunctionOfKind(int kind) const {
  const std::vector<mathprogram::Function>& fns = program_.functions();
  const auto wanted = [kind](const mathprogram::Function& fn) {
    if (kind == 0) return fn.IsPicture() && !fn.IsSurface();
    if (kind == 1) return fn.IsSurface();
    return fn.IsVolume();
  };
  const int read = FunctionShown();
  if (read >= 0 && wanted(fns[static_cast<std::size_t>(read)])) return read;
  for (std::size_t i = 0; i < fns.size(); ++i) {
    if (wanted(fns[i])) return static_cast<int>(i);
  }
  return -1;
}

int CodeBlock::FlatPictureFunction() const { return FunctionOfKind(0); }
int CodeBlock::SurfaceFunction() const { return FunctionOfKind(1); }
int CodeBlock::VolumeFunction() const { return FunctionOfKind(2); }

// One drawing, whichever kind. The window says which function, which patch of
// the input space, and where the eye is; the shader has all three marches in it
// already and `shape` is which one to run.
void CodeBlock::DrawFunctionInto(Rectangle rect, int which, int shape, Vector2 centre,
                                 float span, Vector3 orbit) const {
  if (!haveShader_ || !havePreviewSurface_ || which < 0) return;

  for (const auto& [uniform, location] : uniforms_) {
    if (location < 0) continue;
    const mathprogram::Literal* literal = program_.FindLiteral(uniform);
    if (literal == nullptr) continue;
    SetShaderValue(shader_, location, &literal->value, SHADER_UNIFORM_FLOAT);
  }

  const int   drawMode = 2;
  const float half = std::max(span, 1e-4f) * 0.5f;
  // How far a surface may rise. The colour saturates at one either way, so a
  // couple of units is every height the map can still tell apart -- and it
  // keeps a function that runs away from taking the window with it.
  const float lift = 2.0f;
  const float eye[3] = {orbit.x, orbit.y, orbit.z};
  const float across[2] = {centre.x - half, centre.x + half};
  // Up is up: the top of the picture is the larger value, as it is on a plot.
  const float down[2] = {centre.y + half, centre.y - half};
  if (modeLoc_ >= 0) SetShaderValue(shader_, modeLoc_, &drawMode, SHADER_UNIFORM_INT);
  if (shapeLoc_ >= 0) SetShaderValue(shader_, shapeLoc_, &shape, SHADER_UNIFORM_INT);
  if (indexLoc_ >= 0) SetShaderValue(shader_, indexLoc_, &which, SHADER_UNIFORM_INT);
  if (rangeLoc_ >= 0) SetShaderValue(shader_, rangeLoc_, across, SHADER_UNIFORM_VEC2);
  if (verticalLoc_ >= 0) SetShaderValue(shader_, verticalLoc_, down, SHADER_UNIFORM_VEC2);
  if (orbitLoc_ >= 0) SetShaderValue(shader_, orbitLoc_, eye, SHADER_UNIFORM_VEC3);
  if (liftLoc_ >= 0) SetShaderValue(shader_, liftLoc_, &lift, SHADER_UNIFORM_FLOAT);

  BeginShaderMode(shader_);
  // One white pixel stretched over the square, so the varying runs 0..1 across.
  DrawTexturePro(previewSurface_, {0.0f, 0.0f, 1.0f, 1.0f}, rect, {0.0f, 0.0f}, 0.0f, WHITE);
  EndShaderMode();
}

void CodeBlock::DrawPictureInto(Rectangle rect, Vector2 centre, float span) const {
  DrawFunctionInto(rect, FlatPictureFunction(), 2, centre, span, {0.0f, 0.0f, 0.0f});
}

void CodeBlock::DrawSurfaceInto(Rectangle rect, Vector2 centre, float span,
                                Vector3 orbit) const {
  DrawFunctionInto(rect, SurfaceFunction(), 3, centre, span, orbit);
}

void CodeBlock::DrawVolumeInto(Rectangle rect, Vector2 centre, float span,
                               Vector3 orbit) const {
  DrawFunctionInto(rect, VolumeFunction(), 4, centre, span, orbit);
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
  const float zoom = viewport.Zoom();
  const float emPx = em_ * zoom;
  // The measure, in pixels: how far the words run before they wrap. A box that
  // has never been given one takes a sensible measure from its own type size.
  const float measure = (textWidth_ > 0.0f ? textWidth_ : em_ * kMeasureEms) * zoom;
  EnsureCache(fonts, emPx, measure);

  const Vector2 topLeft = viewport.ToScreen(origin_);

  Shape now;
  now.zoom = zoom;
  now.emPx = emPx;
  now.measure = measure;
  now.left = topLeft.x;
  now.top = topLeft.y;
  now.cache = cacheVersion_;
  // One field each, and no two sharing a bit: showSource at 0, reading at 1-2,
  // focused at 3, justify at 4-5. Two fields overlapping here would be a box
  // that quietly kept a stale layout in exactly one combination.
  now.flags = (showSource_ ? 1u : 0u) | (static_cast<unsigned>(reading_) << 1) |
              (focused_ ? 8u : 0u) | (static_cast<unsigned>(justify_) << 4);
  now.caret = caretLine_;
  now.hover = hoverLine_;
  now.error = ErrorLine();
  // What the injections would say. They are part of how wide a box is, and a
  // knob moving is the one thing that changes them.
  now.values = 2166136261u;
  for (const mathprogram::Variable& variable : program_.variables()) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &variable.value, sizeof(bits));
    now.values = (now.values ^ bits) * 16777619u;
  }

  if (now.SameShape(shape_)) {
    // Panning moves a box without reshaping it, so the rows are shifted rather
    // than measured again.
    const float dx = now.left - shape_.left;
    const float dy = now.top - shape_.top;
    if (dx != 0.0f || dy != 0.0f) {
      for (Row& row : g.rows) {
        row.x += dx;
        row.y += dy;
        row.baseline += dy;
      }
      g.rect.x += dx;
      g.rect.y += dy;
      g.textLeftTop.x += dx;
      g.textLeftTop.y += dy;
      shape_ = now;
    }
    return g;
  }
  shape_ = now;

  g.zoom = zoom;
  g.emPx = emPx;
  g.measure = measure;
  g.advance = fonts.Advance(Face::Mono, 'M', g.emPx);
  g.lineHeight = text::LineHeight(fonts, Face::Mono, g.emPx);
  g.ascent = text::Ascent(fonts, Face::Mono, g.emPx);

  // Padding is in proportion to the type, so growing a box grows the whole
  // card and not just what is written in it.
  const float padScale = em_ / kBaseEm;
  const float padX = CardPad() * padScale * g.zoom;
  const float padY = CardPad() * padScale * g.zoom;

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

      // Indentation is how code says a line is inside something, and the
      // typeset reading has the same thing to say -- a `return` set flush left
      // does not look like it belongs to the function above it. Prose is
      // exempt: there a leading space is a space between words.
      if (asLatex && !IsText()) {
        const std::size_t lead = lines_[i].find_first_not_of(" \t");
        if (lead != std::string::npos && lead > 0) {
          row.indent =
              text::Measure(fonts, Face::Mono, std::string_view(lines_[i]).substr(0, lead), g.emPx);
        }
      }

      float latexExtent = 0.0f;
      if (layout != nullptr) {
        row.width = layout->width;
        latexExtent = layout->height + layout->depth + 6.0f * g.zoom;
        contentWidth = std::max(contentWidth, row.width + row.indent);
        row.baseline = 0.0f;
      } else {
        row.width = text::Measure(fonts, Face::Mono, lines_[i], g.emPx);
      }

      // A line is the same height in *either* reading, so flipping one never
      // changes the shape of the box -- the same rule the width already
      // follows. Taken from the cached layout whether or not this row is the
      // one showing it. Prose is exempt: there the two readings are not two
      // readings of a line but of the whole box, and they do not even have the
      // same number of rows.
      if (!IsText() && !cache_[i].latex.empty()) {
        const tex::Layout& both = cache_[i].layout;
        latexExtent = std::max(latexExtent, both.height + both.depth + 6.0f * g.zoom);
      }

      // And a whole number of dots tall, so a line that a fraction makes taller
      // does not leave every line under it between the rows of dots.
      const float dot = g.lineHeight / kDotsPerLine;
      row.height = std::max(g.lineHeight, latexExtent);
      row.height = std::ceil(row.height / dot - 0.01f) * dot;
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
    row.x = g.textLeftTop.x + offset + row.indent;
  }
  return g;
}

float CodeBlock::PaddingX(float em) { return CardPad() * (em / kBaseEm); }

Vector2 CodeBlock::TextOffset() const {
  const float scale = em_ / kBaseEm;
  return {CardPad() * scale, CardPad() * scale};
}

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
    // One gap between every control, whatever it is: a strip with a rhythm to
    // it reads as one strip, and grouping by spacing was buying nothing now
    // that the three justifications are one button.
    chrome.bold = {x, y, side, side};
    x += side + gap;
    chrome.italic = {x, y, side, side};
    x += side + gap;
    chrome.justify = {x, y, side, side};
    x += side + gap;
    // Then the same control a code box has: show me what this really is. For
    // prose that means the source, sigil and all -- which is the only way to
    // take the sigil back out.
    //
    // It stays with the rest of the strip rather than going to the box's right
    // edge, because showing the source makes a wrapped paragraph as wide as its
    // longest unwrapped line, and a toggle that leaves the screen when pressed
    // is a toggle you cannot press again.
    chrome.toggle = {x, y, kToggleWidth * g.zoom, height};
    return chrome;
  }

  const float toggleWidth = kToggleWidth * g.zoom;
  // The cross that used to sit here is gone: a box is thrown away the same way
  // every other window is, by carrying it onto the drop target.
  chrome.toggle = {g.rect.x + g.rect.width - toggleWidth, y, toggleWidth, height};
  return chrome;
}

bool CodeBlock::ChromeVisible(const Geometry& g, Vector2 pointer) const {
  // The measure being dragged keeps its own bar, however the pointer wanders.
  if (focused_ || marginDrag_) return true;
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

int CodeBlock::RowOf(const Geometry& g, int line, int col) const {
  int fallback = -1;
  for (std::size_t i = 0; i < g.rows.size(); ++i) {
    const Row& row = g.rows[i];
    if (row.line != line) continue;
    if (fallback < 0) fallback = static_cast<int>(i);
    // The first row that contains the offset wins, so a caret sitting exactly
    // on a wrap point shows at the end of a line rather than the start of the
    // next -- which is where you just typed.
    if (static_cast<std::size_t>(col) >= row.begin && static_cast<std::size_t>(col) <= row.end) {
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
  caretUsed_ = true;
  blinkAnchor_ = GetTime();
}

// ------------------------------------------------------------ selecting text

bool CodeBlock::HasSelection() const {
  return anchorLine_ != caretLine_ || anchorCol_ != caretCol_;
}

void CodeBlock::SelectionRange(int* fromLine, int* fromCol, int* toLine, int* toCol) const {
  const bool forwards =
      (anchorLine_ < caretLine_) || (anchorLine_ == caretLine_ && anchorCol_ <= caretCol_);
  *fromLine = forwards ? anchorLine_ : caretLine_;
  *fromCol = forwards ? anchorCol_ : caretCol_;
  *toLine = forwards ? caretLine_ : anchorLine_;
  *toCol = forwards ? caretCol_ : anchorCol_;
}

bool CodeBlock::SelectionInRow(const Row& row, std::size_t* from, std::size_t* to) const {
  if (!HasSelection()) return false;
  int fromLine = 0, fromCol = 0, toLine = 0, toCol = 0;
  SelectionRange(&fromLine, &fromCol, &toLine, &toCol);
  if (row.line < fromLine || row.line > toLine) return false;

  const std::size_t begin =
      (row.line == fromLine) ? std::max<std::size_t>(row.begin, fromCol) : row.begin;
  const std::size_t end =
      (row.line == toLine) ? std::min<std::size_t>(row.end, toCol) : row.end;
  if (end <= begin) return false;
  *from = begin;
  *to = end;
  return true;
}

void CodeBlock::BeginSelection(Vector2 screenPoint, const Viewport& viewport, FontStore& fonts) {
  PlaceCaret(screenPoint, viewport, fonts);
  SelectNone();
}

void CodeBlock::ExtendSelection(Vector2 screenPoint, const Viewport& viewport, FontStore& fonts) {
  const int line = anchorLine_;
  const int col = anchorCol_;
  PlaceCaret(screenPoint, viewport, fonts);
  anchorLine_ = line;
  anchorCol_ = col;
}

bool CodeBlock::PointInSelection(Vector2 screenPoint, const Viewport& viewport,
                                 FontStore& fonts) const {
  if (!HasSelection()) return false;
  const Geometry& g = Measure(viewport, fonts);
  for (const Row& row : g.rows) {
    if (screenPoint.y < row.y || screenPoint.y >= row.y + row.height) continue;
    std::size_t from = 0;
    std::size_t to = 0;
    if (!SelectionInRow(row, &from, &to)) return false;

    const std::string& line = lines_[static_cast<std::size_t>(row.line)];
    const Face  face = row.asLatex ? ProseFace() : Face::Mono;
    const float left =
        row.x + text::Measure(fonts, face, std::string_view(line).substr(row.begin,
                                                                        from - row.begin),
                              g.emPx);
    const float width =
        text::Measure(fonts, face, std::string_view(line).substr(from, to - from), g.emPx);
    return screenPoint.x >= left && screenPoint.x <= left + width;
  }
  return false;
}

std::string CodeBlock::SelectedVariable() const {
  if (!HasSelection()) return {};
  int fromLine = 0, fromCol = 0, toLine = 0, toCol = 0;
  SelectionRange(&fromLine, &fromCol, &toLine, &toCol);
  // A name is written on one line, and what is selected has to be part of it
  // and nothing else -- otherwise "a + b" would name something.
  if (fromLine != toLine) return {};
  const std::string& line = lines_[static_cast<std::size_t>(fromLine)];
  const auto isWord = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
  };
  for (int i = fromCol; i < toCol; ++i) {
    if (!isWord(line[static_cast<std::size_t>(i)])) return {};
  }

  // Grown to the whole identifier: selecting one letter of a name is naming it.
  std::size_t begin = static_cast<std::size_t>(fromCol);
  std::size_t end = static_cast<std::size_t>(toCol);
  while (begin > 0 && isWord(line[begin - 1])) --begin;
  while (end < line.size() && isWord(line[end])) ++end;
  if (end <= begin) return {};

  const std::string word = line.substr(begin, end - begin);
  for (const mathprogram::Variable& variable : program_.variables()) {
    if (variable.name == word && variable.kind == mathprogram::Kind::Real) return word;
  }
  return {};
}

bool CodeBlock::EraseSelection() {
  if (!HasSelection()) return false;
  PushUndo(EditKind::Structural);

  int fromLine = 0, fromCol = 0, toLine = 0, toCol = 0;
  SelectionRange(&fromLine, &fromCol, &toLine, &toCol);
  const std::size_t first = static_cast<std::size_t>(fromLine);
  const std::size_t last = static_cast<std::size_t>(toLine);

  if (first == last) {
    lines_[first].erase(static_cast<std::size_t>(fromCol),
                        static_cast<std::size_t>(toCol - fromCol));
  } else {
    const std::string head = lines_[first].substr(0, static_cast<std::size_t>(fromCol));
    const std::string tail = lines_[last].substr(static_cast<std::size_t>(toCol));
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(first) + 1,
                 lines_.begin() + static_cast<std::ptrdiff_t>(last) + 1);
    lines_[first] = head + tail;
  }
  caretLine_ = fromLine;
  caretCol_ = fromCol;
  // Clamped before the edit is signed off, so the anchor lands wherever the
  // caret actually ended up.
  ClampCaret();
  TouchEdit();
  return true;
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

std::string CodeBlock::SelectedText() const {
  if (!HasSelection()) return {};

  int fromLine = 0;
  int fromCol = 0;
  int toLine = 0;
  int toCol = 0;
  SelectionRange(&fromLine, &fromCol, &toLine, &toCol);

  std::string out;
  for (int i = fromLine; i <= toLine && i < static_cast<int>(lines_.size()); ++i) {
    const std::string& line = lines_[static_cast<std::size_t>(i)];
    const std::size_t  begin =
        (i == fromLine) ? std::min(static_cast<std::size_t>(fromCol), line.size()) : 0;
    const std::size_t end =
        (i == toLine) ? std::min(static_cast<std::size_t>(toCol), line.size()) : line.size();
    if (i != fromLine) out += '\n';
    if (begin < end) out += line.substr(begin, end - begin);
  }
  return out;
}

void CodeBlock::CopyText(bool cut) {
  const std::string taken = SelectedText();
  if (taken.empty()) return;   // nothing highlighted is nothing to take
  SetClipboardText(taken.c_str());
  if (!cut) return;
  PushUndo(EditKind::Structural);
  EraseSelection();
}

void CodeBlock::PasteText() {
  const char* text = GetClipboardText();
  if (text == nullptr || *text == '\0') return;

  PushUndo(EditKind::Structural);
  // Pasting over a highlight replaces it, which is what a highlight is for.
  EraseSelection();

  // Inserted as bytes rather than codepoint by codepoint: the caret is a byte
  // offset and what arrives is UTF-8, so whole sequences go in as they are.
  std::string chunk;
  for (const char* c = text;; ++c) {
    if (*c != '\0' && *c != '\n') {
      if (*c != '\r') chunk += *c;
      continue;
    }
    if (!chunk.empty()) {
      std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
      line.insert(static_cast<std::size_t>(caretCol_), chunk);
      caretCol_ += static_cast<int>(chunk.size());
      chunk.clear();
      // The anchor comes with it. Left where it was, the text just inserted
      // would be *highlighted* -- and the newline below erases a highlight
      // before it splits the line, so each chunk would be deleted by the one
      // after it.
      SelectNone();
    }
    if (*c == '\0') break;
    InsertNewline();
  }
  ClampCaret();
  TouchEdit();
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
      // Where the box is being read decides *which* literal a drag moves: the
      // one responsible for the number on screen. Read above a reassignment and
      // the knob moves the declaration's literal; read below it, the
      // reassignment's.
      program_.SetValue(i, value, ExecutedUpTo());
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
  SelectNone();
  dirty_ = true;
  lastEdit_ = EditKind::None;
  blinkAnchor_ = GetTime();
}

void CodeBlock::TouchEdit() {
  dirty_ = true;
  caretUsed_ = true;
  // Every edit ends with nothing selected. The anchor is only ever the far end
  // of a selection being made; left where it was, each keystroke would select
  // everything typed since and the next one would replace it.
  anchorLine_ = caretLine_;
  anchorCol_ = caretCol_;
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
  // Typing over a selection replaces it, which is what a selection is for.
  EraseSelection();
  PushUndo(EditKind::Insert);
  const std::string encoded = EncodeUtf8(codepoint);
  std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  line.insert(static_cast<std::size_t>(caretCol_), encoded);
  caretCol_ += static_cast<int>(encoded.size());
  TouchEdit();
}

void CodeBlock::InsertNewline() {
  EraseSelection();
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
  if (EraseSelection()) return;
  std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  if (caretCol_ > FloorCol(caretLine_)) {
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
  if (EraseSelection()) return;
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
  // Moving the caret puts the highlight down -- unless shift is held, which is
  // the whole of what shift means: keep the anchor, move the caret, and the
  // stretch between them is highlighted.
  if (!ShiftDown()) SelectNone();
  caretUsed_ = true;
  blinkAnchor_ = GetTime();
}

int CodeBlock::FloorCol(int line) const {
  if (line < 0 || line >= static_cast<int>(lines_.size())) return 0;
  if (!IsText() || showSource_) return 0;
  return static_cast<int>(ProseOffset(static_cast<std::size_t>(line)));
}

int CodeBlock::WordBoundary(int direction) const {
  const std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  const int          size = static_cast<int>(line.size());
  int                i = caretCol_;

  if (direction < 0) {
    // Back over any run of spaces, then over the word before it.
    while (i > 0 && !IsWordChar(line[static_cast<std::size_t>(i - 1)])) --i;
    while (i > 0 && IsWordChar(line[static_cast<std::size_t>(i - 1)])) --i;
    // A word back from the first word is still not the sigil.
    return std::max(i, FloorCol(caretLine_));
  }
  while (i < size && IsWordChar(line[static_cast<std::size_t>(i)])) ++i;
  while (i < size && !IsWordChar(line[static_cast<std::size_t>(i)])) ++i;
  return i;
}

void CodeBlock::MoveCaretByWord(int direction) {
  if (direction < 0 && caretCol_ <= FloorCol(caretLine_)) {
    MoveCaret(0, -1);
    return;
  }
  if (direction > 0 && caretCol_ >= static_cast<int>(lines_[
          static_cast<std::size_t>(caretLine_)].size())) {
    MoveCaret(0, +1);
    return;
  }
  caretCol_ = WordBoundary(direction);
  ClampCaret();
  if (!ShiftDown()) SelectNone();
  caretUsed_ = true;
  blinkAnchor_ = GetTime();
}

// Ctrl and a delete take a whole word, which is what Ctrl and an arrow already
// move by. At the start of a line there is no word to take, so the line break
// is what goes -- the same thing the plain key would have done.
void CodeBlock::DeleteWordBefore() {
  if (EraseSelection()) return;
  const int floor = FloorCol(caretLine_);
  if (caretCol_ <= floor) {
    DeleteBefore();
    return;
  }
  PushUndo(EditKind::Delete);
  const int    start = WordBoundary(-1);
  std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  line.erase(static_cast<std::size_t>(start), static_cast<std::size_t>(caretCol_ - start));
  caretCol_ = start;
  TouchEdit();
}

void CodeBlock::DeleteWordAfter() {
  if (EraseSelection()) return;
  std::string& line = lines_[static_cast<std::size_t>(caretLine_)];
  if (caretCol_ >= static_cast<int>(line.size())) {
    DeleteAt();
    return;
  }
  PushUndo(EditKind::Delete);
  const int end = WordBoundary(+1);
  line.erase(static_cast<std::size_t>(caretCol_), static_cast<std::size_t>(end - caretCol_));
  TouchEdit();
}

void CodeBlock::HandleInput() {
  const bool ctrl = CtrlDown();

  if (ctrl && Triggered(KEY_Z)) {
    Undo();
    return;
  }

  // The caret is what says these are about the text rather than about the
  // windows on the canvas.
  if (ctrl && Triggered(KEY_C)) {
    CopyText(/*cut=*/false);
    return;
  }
  if (ctrl && Triggered(KEY_X)) {
    CopyText(/*cut=*/true);
    return;
  }
  if (ctrl && Triggered(KEY_V)) {
    PasteText();
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
  if (Triggered(KEY_BACKSPACE)) ctrl ? DeleteWordBefore() : DeleteBefore();
  if (Triggered(KEY_DELETE)) ctrl ? DeleteWordAfter() : DeleteAt();
  if (Triggered(KEY_LEFT)) MoveCaret(0, -1);
  if (Triggered(KEY_RIGHT)) MoveCaret(0, +1);
  if (Triggered(KEY_UP)) MoveCaret(-1, 0);
  if (Triggered(KEY_DOWN)) MoveCaret(+1, 0);
  if (Triggered(KEY_TAB)) {
    for (int i = 0; i < 4; ++i) InsertCodepoint(' ');
  }
  if (IsKeyPressed(KEY_HOME)) {
    caretCol_ = 0;
    // The start of the line is the start of what you are *writing*, which in a
    // text box is past the sigil. Every other way of moving the caret went
    // through this; Home did not, so Home could put it somewhere the caret is
    // not allowed to be -- and a highlight made from there took the sigil with
    // it when it was copied.
    ClampCaret();
    if (!ShiftDown()) SelectNone();
    caretUsed_ = true;
    blinkAnchor_ = GetTime();
  }
  if (IsKeyPressed(KEY_END)) {
    caretCol_ = static_cast<int>(lines_[static_cast<std::size_t>(caretLine_)].size());
    ClampCaret();
    if (!ShiftDown()) SelectNone();
    caretUsed_ = true;
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
    const float padX = CardPad() * (em_ / kBaseEm) * zoom;
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
    if (CheckCollisionPointRec(pointer, chrome.justify)) {
      // Pressed until it says what you want.
      if (pressed) justify_ = static_cast<Justify>((static_cast<int>(justify_) + 1) % 3);
      return true;
    }
    if (CheckCollisionPointRec(pointer, chrome.toggle)) {
      if (pressed) showSource_ = !showSource_;
      return true;
    }
    return false;
  }

  if (!CheckCollisionPointRec(pointer, chrome.toggle)) return false;
  // Pressing it settles the question the other way, and it stays settled.
  if (pressed) reading_ = ReadsAsCode() ? Reading::Maths : Reading::Code;
  return true;   // claim the hover too, so a click never falls through
}

void CodeBlock::RequestCompile() {
  dirty_ = false;
  // A text box is not a program. Nothing about words compiles, and trying
  // would put a diagnostic on a sentence.
  program_.Rebuild(IsText() ? std::vector<std::string>{} : lines_);
}

void CodeBlock::ReleaseGpu() {
  if (haveShader_) {
    UnloadShader(shader_);
    haveShader_ = false;
  }
  if (haveVizShader_) {
    UnloadShader(vizShader_);
    haveVizShader_ = false;
  }
  if (haveShader_) {
    UnloadShader(shader_);
    haveShader_ = false;
  }
  if (traceBuffer_ != 0) {
    rlUnloadShaderBuffer(traceBuffer_);
    traceBuffer_ = 0;
    traceFloats_ = 0;
  }
  if (fnInBuffer_ != 0) { rlUnloadShaderBuffer(fnInBuffer_); fnInBuffer_ = 0; }
  if (fnOutBuffer_ != 0) { rlUnloadShaderBuffer(fnOutBuffer_); fnOutBuffer_ = 0; }
  if (haveFnTarget_) { UnloadRenderTexture(fnTarget_); haveFnTarget_ = false; }
  if (samplerProgram_ != 0) { rlUnloadShaderProgram(samplerProgram_); samplerProgram_ = 0; }
  if (samplerBuffer_ != 0) { rlUnloadShaderBuffer(samplerBuffer_); samplerBuffer_ = 0; }
  if (vizMeshProgram_ != 0) {
    rlUnloadShaderProgram(vizMeshProgram_);
    vizMeshProgram_ = 0;
  }
  if (vizTriBuffer_ != 0) { rlUnloadShaderBuffer(vizTriBuffer_); vizTriBuffer_ = 0; }
  if (vizLineBuffer_ != 0) { rlUnloadShaderBuffer(vizLineBuffer_); vizLineBuffer_ = 0; }
  vizMeshRun_ = false;
  if (haveProbeTarget_) {
    UnloadRenderTexture(probeTarget_);
    haveProbeTarget_ = false;
  }
  if (havePreviewSurface_) {
    UnloadTexture(previewSurface_);
    havePreviewSurface_ = false;
  }
  uniforms_.clear();
  uniforms_.clear();
  probeSignature_.clear();
}

void CodeBlock::RunVizMesh(float value) const {
  if (vizMeshProgram_ == 0 || vizTriBuffer_ == 0 || vizLineBuffer_ == 0) return;
  // The geometry only changes when the value does, so neither does the work.
  if (vizMeshRun_ && value == vizMeshValue_) return;
  vizMeshValue_ = value;
  vizMeshRun_ = true;

  // The counters lead each buffer, and start again every time.
  const unsigned int zero = 0;
  rlUpdateShaderBuffer(vizTriBuffer_, &zero, sizeof(zero), 0);
  rlUpdateShaderBuffer(vizLineBuffer_, &zero, sizeof(zero), 0);

  rlEnableShader(vizMeshProgram_);
  if (vizMeshValueLoc_ >= 0) {
    rlSetUniform(vizMeshValueLoc_, &value, RL_SHADER_UNIFORM_FLOAT, 1);
  }
  rlBindShaderBuffer(vizTriBuffer_, 1);
  rlBindShaderBuffer(vizLineBuffer_, 2);
  rlComputeShaderDispatch(1, 1, 1);
  rlDisableShader();

  // What compute wrote is not visible to the vertex fetch that reads it until
  // this says so. Without it the only way to wait was to copy the whole mesh
  // back to the CPU, which is the stall the buffer existed to avoid.
  glext::Barrier(glext::kShaderStorage | glext::kVertexAttribArray | glext::kBufferUpdate);

  // Only the counts come back -- four bytes each, and only when the value has
  // moved. A draw call has to be told how many vertices there are, and short of
  // an indirect draw there is nowhere else for that number to come from.
  unsigned int counts[2] = {0, 0};
  rlReadShaderBuffer(vizTriBuffer_, &counts[0], sizeof(unsigned int), 0);
  rlReadShaderBuffer(vizLineBuffer_, &counts[1], sizeof(unsigned int), 0);
  const int cap = mathprogram::Program::kVizVertexCap;
  vizTriCount_ = std::min<int>(static_cast<int>(counts[0]), cap);
  vizLineCount_ = std::min<int>(static_cast<int>(counts[1]), cap);
}

const std::vector<float>& CodeBlock::SamplerRun() const {
  // The spread is a function of the box's lines and nothing else, so it is laid
  // down once and then left alone.
  if (samplerRan_ || samplerProgram_ == 0 || samplerBuffer_ == 0) return samplerRun_;
  samplerRan_ = true;

  const unsigned int zero = 0;
  rlUpdateShaderBuffer(samplerBuffer_, &zero, sizeof(zero), 0);
  rlEnableShader(samplerProgram_);
  rlBindShaderBuffer(samplerBuffer_, 1);
  rlComputeShaderDispatch(1, 1, 1);
  rlDisableShader();
  glext::Barrier(glext::kShaderStorage | glext::kBufferUpdate);

  unsigned int laid = 0;
  rlReadShaderBuffer(samplerBuffer_, &laid, sizeof(laid), 0);
  const int count = std::min<int>(static_cast<int>(laid), program_.samplerCount());
  const int floats = count * program_.samplerStride();
  samplerRun_.assign(static_cast<std::size_t>(std::max(0, floats)), 0.0f);
  if (floats > 0) {
    // Past the counter, which leads the buffer.
    rlReadShaderBuffer(samplerBuffer_, samplerRun_.data(),
                       static_cast<unsigned int>(floats * static_cast<int>(sizeof(float))),
                       static_cast<unsigned int>(sizeof(unsigned int)));
  }
  return samplerRun_;
}

std::vector<std::string> CodeBlock::SampledTypes() const {
  std::vector<std::string> out;
  for (const mathprogram::Function& fn : program_.functions()) {
    if (!fn.sampled) continue;
    for (const std::string& type : {fn.inputType, fn.outputType}) {
      if (std::find(out.begin(), out.end(), type) == out.end()) out.push_back(type);
    }
  }
  return out;
}

int CodeBlock::FunctionShown() const {
  return program_.FunctionAt(ExecutedUpTo());
}

void CodeBlock::SetSampleInputs(const std::vector<float>& inputs, int count, int stride) {
  const int room = mathprogram::Program::kMaxSamples;
  const int many = std::clamp(count, 0, room);
  if (static_cast<int>(inputs.size()) < many * stride) return;
  if (sampling_.inputs.size() == static_cast<std::size_t>(many * stride) &&
      std::equal(inputs.begin(), inputs.begin() + many * stride, sampling_.inputs.begin())) {
    return;
  }
  sampling_.inputs.assign(inputs.begin(), inputs.begin() + many * stride);
  sampling_.count = many;
  sampling_.inStride = std::max(1, stride);
  sampling_.ready = false;
}

// Putting the spread through the function. One fragment per sample, in a mode
// that returns before the body -- what a function does is in its definition,
// and the box's own lines have nothing to say about it.
void CodeBlock::FeedFunction() {
  const int which = FunctionShown();
  if (which < 0) {
    // Nothing is being read inside a function, so there is nothing to show.
    // The spread itself is kept: it is the canvas's, not this moment's.
    sampling_.ready = false;
    return;
  }
  if (!haveShader_ || !haveFnTarget_ || !havePreviewSurface_) return;
  const mathprogram::Function& fn = program_.functions()[static_cast<std::size_t>(which)];
  sampling_.name = fn.name;
  sampling_.inputType = fn.inputType;
  sampling_.outputType = fn.outputType;
  sampling_.inStride = fn.inStride;
  sampling_.outStride = fn.outStride;
  if (sampling_.count <= 0 || fnInBuffer_ == 0 || fnOutBuffer_ == 0) {
    sampling_.ready = false;
    return;
  }

  // Nothing has moved, so the answers on screen are still the answers.
  std::vector<float> signature = sampling_.inputs;
  signature.push_back(static_cast<float>(which));
  for (const auto& [uniform, location] : uniforms_) {
    const mathprogram::Literal* literal = program_.FindLiteral(uniform);
    signature.push_back(literal == nullptr ? 0.0f : literal->value);
  }
  if (signature == fnSignature_ && sampling_.ready) return;
  fnSignature_ = signature;

  rlUpdateShaderBuffer(
      fnInBuffer_, sampling_.inputs.data(),
      static_cast<unsigned int>(sampling_.inputs.size() * sizeof(float)), 0);

  for (const auto& [uniform, location] : uniforms_) {
    if (location < 0) continue;
    const mathprogram::Literal* literal = program_.FindLiteral(uniform);
    if (literal == nullptr) continue;
    SetShaderValue(shader_, location, &literal->value, SHADER_UNIFORM_FLOAT);
  }
  const int feedMode = 3;
  if (modeLoc_ >= 0) SetShaderValue(shader_, modeLoc_, &feedMode, SHADER_UNIFORM_INT);
  if (fnIndexLoc_ >= 0) SetShaderValue(shader_, fnIndexLoc_, &which, SHADER_UNIFORM_INT);
  if (fnCountLoc_ >= 0) {
    SetShaderValue(shader_, fnCountLoc_, &sampling_.count, SHADER_UNIFORM_INT);
  }

  rlBindShaderBuffer(fnInBuffer_, 3);
  rlBindShaderBuffer(fnOutBuffer_, 4);

  BeginTextureMode(fnTarget_);
  rlDisableColorBlend();
  BeginShaderMode(shader_);
  DrawTexturePro(previewSurface_, {0.0f, 0.0f, 1.0f, 1.0f},
                 {0.0f, 0.0f, static_cast<float>(sampling_.count), 1.0f}, {0.0f, 0.0f}, 0.0f,
                 WHITE);
  EndShaderMode();
  rlEnableColorBlend();
  EndTextureMode();

  glext::Barrier(glext::kShaderStorage | glext::kBufferUpdate);

  const int floats = sampling_.count * sampling_.outStride;
  sampling_.outputs.assign(static_cast<std::size_t>(floats), 0.0f);
  rlReadShaderBuffer(fnOutBuffer_, sampling_.outputs.data(),
                     static_cast<unsigned int>(floats * static_cast<int>(sizeof(float))), 0);
  sampling_.ready = true;
}

int CodeBlock::ExecutedUpTo() const {
  // The caret's line while the box is being written in, and otherwise the end.
  // A box nobody has selected is not being read a line at a time, so what its
  // windows show is what the whole box came to -- not wherever the pointer
  // happens to be passing, and not where the caret was left last time.
  if (focused_ && caretUsed_) return caretLine_;
  return -1;   // the end
}

void CodeBlock::ProbeIfNeeded() {
  if (!haveShader_ || !haveProbeTarget_ || !havePreviewSurface_) return;

  // Nothing has moved, so the answer is the one already on screen.
  std::vector<float> signature;
  signature.reserve(uniforms_.size() + 2);
  signature.push_back(probePoint_.x);
  signature.push_back(probePoint_.y);
  for (const auto& [uniform, location] : uniforms_) {
    const mathprogram::Literal* literal = program_.FindLiteral(uniform);
    signature.push_back(literal == nullptr ? 0.0f : literal->value);
  }
  if (signature == probeSignature_) return;
  probeSignature_ = signature;

  for (const auto& [uniform, location] : uniforms_) {
    if (location < 0) continue;
    const mathprogram::Literal* literal = program_.FindLiteral(uniform);
    if (literal == nullptr) continue;
    SetShaderValue(shader_, location, &literal->value, SHADER_UNIFORM_FLOAT);
  }
  if (probePointLoc_ >= 0) {
    const float point[2] = {probePoint_.x, probePoint_.y};
    SetShaderValue(shader_, probePointLoc_, point, SHADER_UNIFORM_VEC2);
  }
  const int reportMode = 1;
  if (modeLoc_ >= 0) SetShaderValue(shader_, modeLoc_, &reportMode, SHADER_UNIFORM_INT);

  if (traceBuffer_ != 0) rlBindShaderBuffer(traceBuffer_, 0);

  const float width = static_cast<float>(probeTarget_.texture.width);
  BeginTextureMode(probeTarget_);
  // The pixels are data, not a picture: blending them with what was there
  // before would corrupt every byte.
  rlDisableColorBlend();
  BeginShaderMode(shader_);
  DrawTexturePro(previewSurface_, {0.0f, 0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, width, 1.0f},
                 {0.0f, 0.0f}, 0.0f, WHITE);
  EndShaderMode();
  rlEnableColorBlend();
  EndTextureMode();

  if (traceBuffer_ != 0 && traceFloats_ > 0) {
    std::vector<float> values(static_cast<std::size_t>(traceFloats_), 0.0f);
    rlReadShaderBuffer(traceBuffer_, values.data(),
                       static_cast<unsigned int>(traceFloats_ * static_cast<int>(sizeof(float))),
                       0);
    program_.TakeTrace(std::move(values));
  }

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

  // A viz box's GLSL is the *vertex* shader -- the one that decides where the
  // mesh ends up. Nothing else about it is different: it is assembled from the
  // same lines, compiled on the same thread, and its errors come back the same
  // way, because it is still just the box's text tried as the other language.
  if (!program_.vizType().empty()) {
    if (haveVizShader_) {
      UnloadShader(vizShader_);
      haveVizShader_ = false;
    }

    // The half that lays geometry down is a compute program, and it needs
    // somewhere to lay it: two buffers, one for triangles and one for the
    // segments of a path, each with its count in front of it.
    if (!program_.glslMeshSource().empty()) {
      if (vizMeshProgram_ != 0) { rlUnloadShaderProgram(vizMeshProgram_); vizMeshProgram_ = 0; }
      const unsigned int compiled =
          rlCompileShader(program_.glslMeshSource().c_str(), RL_COMPUTE_SHADER);
      if (compiled != 0) vizMeshProgram_ = rlLoadComputeShaderProgram(compiled);
      if (vizMeshProgram_ != 0) {
        vizMeshValueLoc_ = rlGetLocationUniform(vizMeshProgram_, "value");
        const unsigned int bytes = static_cast<unsigned int>(
            sizeof(unsigned int) +
            static_cast<std::size_t>(mathprogram::Program::kVizVertexCap) * 2u * sizeof(float));
        if (vizTriBuffer_ == 0) vizTriBuffer_ = rlLoadShaderBuffer(bytes, nullptr, RL_DYNAMIC_COPY);
        if (vizLineBuffer_ == 0) vizLineBuffer_ = rlLoadShaderBuffer(bytes, nullptr, RL_DYNAMIC_COPY);
        vizMeshRun_ = false;
      }
      program_.ShaderCompiled(vizMeshProgram_ != 0,
                              vizMeshProgram_ != 0 ? std::string() : "compute shader failed");
      return;
    }

    haveVizShader_ =
        CompileShaderHalf(program_.glslSource().c_str(), nullptr, &vizShader_, &log);
    program_.ShaderCompiled(haveVizShader_, log);
    return;
  }

  // A sampler box lays a spread of points down, so like a viz's mesh half it is
  // a compute program writing into a buffer through an atomic counter.
  if (!program_.samplerType().empty()) {
    if (samplerProgram_ != 0) { rlUnloadShaderProgram(samplerProgram_); samplerProgram_ = 0; }
    const unsigned int compiled =
        rlCompileShader(program_.glslSampleSource().c_str(), RL_COMPUTE_SHADER);
    if (compiled != 0) samplerProgram_ = rlLoadComputeShaderProgram(compiled);
    if (samplerProgram_ != 0 && samplerBuffer_ == 0) {
      const unsigned int bytes = static_cast<unsigned int>(
          sizeof(unsigned int) +
          static_cast<std::size_t>(mathprogram::Program::kMaxSamples) *
              static_cast<std::size_t>(mathprogram::Program::kMaxStride) * sizeof(float));
      samplerBuffer_ = rlLoadShaderBuffer(bytes, nullptr, RL_DYNAMIC_COPY);
    }
    samplerRan_ = false;
    program_.ShaderCompiled(samplerProgram_ != 0,
                            samplerProgram_ != 0 ? std::string() : "compute shader failed");
    return;
  }

  const bool ok = CompileShaderHalf(program_.glslVertexSource().c_str(),
                                    program_.glslSource().c_str(), &shader_, &log);
  haveShader_ = ok;

  if (ok) {
    // The knobs drive uniforms. Their slots are looked up once here rather than
    // every frame: each one is a round trip to the driver.
    for (const mathprogram::Variable& variable : program_.variables()) {
      if (!variable.free || variable.kind != mathprogram::Kind::Real) continue;
      for (const mathprogram::Literal& literal : variable.literals) {
        uniforms_.emplace_back(literal.uniform,
                               GetShaderLocation(shader_, literal.uniform.c_str()));
      }
    }
    if (!havePreviewSurface_) {
      // Something for the shader to cover. One white pixel stretched over the
      // preview is what makes fragTexCoord run 0..1 across it.
      Image white = GenImageColor(1, 1, WHITE);
      previewSurface_ = LoadTextureFromImage(white);
      UnloadImage(white);
      havePreviewSurface_ = true;
    }

    // The same shader answers the other two questions, so there is nothing
    // else to compile -- only the rest of its slots to look up.
    modeLoc_ = GetShaderLocation(shader_, "sbMode");
    probePointLoc_ = GetShaderLocation(shader_, "sbProbe");
    rangeLoc_ = GetShaderLocation(shader_, "sbRange");
    verticalLoc_ = GetShaderLocation(shader_, "sbVertical");
    indexLoc_ = GetShaderLocation(shader_, "sbIndex");
    shapeLoc_ = GetShaderLocation(shader_, "sbShape");
    thickLoc_ = GetShaderLocation(shader_, "sbThick");
    orbitLoc_ = GetShaderLocation(shader_, "sbOrbit");
    liftLoc_ = GetShaderLocation(shader_, "sbLift");

    // And the slots for feeding a function, plus the two buffers and the strip
    // of fragments that does it -- one fragment per sample.
    fnIndexLoc_ = GetShaderLocation(shader_, "sbFnIndex");
    fnCountLoc_ = GetShaderLocation(shader_, "sbFnCount");
    if (!program_.functions().empty()) {
      const unsigned int bytes = static_cast<unsigned int>(
          static_cast<std::size_t>(mathprogram::Program::kMaxSamples) *
          static_cast<std::size_t>(mathprogram::Program::kMaxStride) * sizeof(float));
      if (fnInBuffer_ == 0) fnInBuffer_ = rlLoadShaderBuffer(bytes, nullptr, RL_DYNAMIC_COPY);
      if (fnOutBuffer_ == 0) fnOutBuffer_ = rlLoadShaderBuffer(bytes, nullptr, RL_DYNAMIC_COPY);
      if (!haveFnTarget_) {
        fnTarget_ = LoadRenderTexture(mathprogram::Program::kMaxSamples, 1);
        haveFnTarget_ = true;
      }
    }
    fnSignature_.clear();
    sampling_ = {};

    // One pixel of target for each derived value it has to report.
    const std::vector<std::string>& probed = program_.probedNames();
    probeTarget_ = LoadRenderTexture(std::max<int>(1, static_cast<int>(probed.size())), 1);
    haveProbeTarget_ = true;
    probeSignature_.clear();

    // One buffer for the whole trace: every real, after every line, written by
    // the single invocation that runs the box. A shader storage buffer rather
    // than a pixel each, because there is no packing to undo and no limit at
    // four bytes.
    {
      const int floats = program_.tracedLines() *
                         static_cast<int>(program_.tracedSlots().size());
      if (floats > 0) {
        if (traceBuffer_ != 0) rlUnloadShaderBuffer(traceBuffer_);
        traceFloats_ = floats;
        // Not zero: a slot for a variable that is not in scope yet has no
        // value, and zero is a value. Left as a NaN, it is skipped rather than
        // reported as nothing.
        const std::vector<float> blank(static_cast<std::size_t>(floats),
                                       std::numeric_limits<float>::quiet_NaN());
        traceBuffer_ = rlLoadShaderBuffer(
            static_cast<unsigned int>(floats * static_cast<int>(sizeof(float))), blank.data(),
            RL_DYNAMIC_COPY);
      }
    }
  }

  program_.ShaderCompiled(ok, log);
}

bool CodeBlock::BeginField(const std::string& name, int mode, Vector2 range, Vector2 vertical,
                           float halfWidth) const {
  if (!haveShader_) return false;

  const std::vector<std::string>& names = program_.functionNames();
  int index = -1;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (names[i] == name) { index = static_cast<int>(i); break; }
  }
  if (index < 0) return false;

  // Whatever its knobs are at. A function's shape can depend on them, which is
  // the whole reason it is drawn by being called.
  for (const auto& [uniform, location] : uniforms_) {
    if (location < 0) continue;
    const mathprogram::Literal* literal = program_.FindLiteral(uniform);
    if (literal == nullptr) continue;
    SetShaderValue(shader_, location, &literal->value, SHADER_UNIFORM_FLOAT);
  }
  const float rangePair[2] = {range.x, range.y};
  const float verticalPair[2] = {vertical.x, vertical.y};
  const int functionMode = 2;
  SetShaderValue(shader_, modeLoc_, &functionMode, SHADER_UNIFORM_INT);
  SetShaderValue(shader_, rangeLoc_, rangePair, SHADER_UNIFORM_VEC2);
  SetShaderValue(shader_, verticalLoc_, verticalPair, SHADER_UNIFORM_VEC2);
  SetShaderValue(shader_, indexLoc_, &index, SHADER_UNIFORM_INT);
  SetShaderValue(shader_, shapeLoc_, &mode, SHADER_UNIFORM_INT);
  SetShaderValue(shader_, thickLoc_, &halfWidth, SHADER_UNIFORM_FLOAT);

  BeginShaderMode(shader_);
  return true;
}

void CodeBlock::EndField() const { EndShaderMode(); }

void CodeBlock::Poll() {
  // What the windows show is what the values were where the box is being read.
  program_.ApplyTrace(ExecutedUpTo());

  // The C++ compile runs on a worker thread, but a shader has to be compiled on
  // the thread holding the graphics context. So this half happens here.
  const mathprogram::State state = program_.state();
  if (state == mathprogram::State::Shading) CompileShader();

  // A box edited into something that no longer compiles should stop drawing the
  // picture it used to be. While a rebuild is in flight the old one stays up,
  // the way the knobs keep their old values.
  if ((haveShader_ || haveVizShader_) && state != mathprogram::State::ShaderReady &&
      state != mathprogram::State::Shading) {
    ReleaseGpu();
  }

  // Ask the shader what its derived variables came to, so the number lines have
  // something to show. The point they are read at was settled while drawing the
  // last frame, which is close enough to be imperceptible.
  ProbeIfNeeded();
  FeedFunction();
}

void CodeBlock::Draw(const Viewport& viewport, FontStore& fonts) const {
  // Hover is resolved here so the next Measure sees it; a line under the
  // pointer shows its code instead of its LaTeX.
  hoverLine_ = -1;
  const Geometry& probe = Measure(viewport, fonts);
  if (probe.emPx < 0.4f) return;
  const Vector2 pointer = hover::Pointer();
  rimHot_ = PointOnRim(probe.rect, pointer);
  // Reaching for the rim should not flip the line underneath it to code.
  if (!rimHot_ && CheckCollisionPointRec(pointer, probe.rect)) {
    const int row = RowAt(pointer, viewport, fonts);
    if (row >= 0 && row < static_cast<int>(probe.rows.size())) {
      hoverLine_ = probe.rows[static_cast<std::size_t>(row)].line;
      // Pointing at a line is asking what things were once it had run.
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
    for (const ActiveValue& active : highlights_) {
      const Color tint = VariableColor(active.name);
      for (const Rectangle& box : MentionRects(r, active.name, g, fonts)) {
        DrawRectangleRounded(box, 0.30f, 6, Fade(tint, kMentionFillAlpha));
        DrawRectangleRoundedLinesEx(box, 0.30f, 6, std::max(1.0f, 1.1f * g.zoom), tint);
      }
    }

    // What is selected, behind the letters.
    if (focused_) {
      std::size_t from = 0;
      std::size_t to = 0;
      if (SelectionInRow(row, &from, &to)) {
        const std::string& source = lines_[i];
        const Face  face = row.asLatex ? ProseFace() : Face::Mono;
        const float left =
            row.x + text::Measure(fonts, face,
                                  std::string_view(source).substr(row.begin, from - row.begin),
                                  g.emPx);
        const float width = text::Measure(
            fonts, face, std::string_view(source).substr(from, to - from), g.emPx);
        DrawRectangleRec({left, row.y + row.height * 0.06f, width, row.height * 0.88f},
                         Fade(kAccent, asWords ? 0.22f : 0.34f));
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
    // A value's injection goes before the semicolon; a body *replaces* it.
    const bool        body = cache_[i].parsed.kind == mathline::Kind::Prototype;
    const std::size_t resume = (body && split < source.size()) ? split + 1 : split;

    float x = row.x;
    x += DrawSpans(fonts, source, spans, 0, split, {x, row.baseline}, g.emPx);
    text::Draw(fonts, Face::Mono, injection, {x, row.baseline}, g.emPx, kInjected);
    x += text::Measure(fonts, Face::Mono, injection, g.emPx);
    DrawSpans(fonts, source, spans, resume, source.size(), {x, row.baseline}, g.emPx);
  }

  // Caret. While this box is being edited, and solid while typing so it never
  // distracts mid-edit -- and also while a number line is offering to put a
  // value in, since then the caret is what says where that value would land.
  int markLine = -1;
  int markCol = 0;
  if (focused_) {
    markLine = caretLine_;
    markCol = caretCol_;
  } else if (insertCaret_) {
    markLine = InsertLine();
    markCol = caretUsed_
                  ? caretCol_
                  : static_cast<int>(lines_[static_cast<std::size_t>(markLine)].size());
  }
  const int caretRow = markLine < 0 ? -1 : RowOf(g, markLine, markCol);
  if (caretRow >= 0) {
    const double phase = std::fmod(GetTime() - blinkAnchor_, 1.06);
    if (phase < 0.62) {
      const Row& row = g.rows[static_cast<std::size_t>(caretRow)];
      // A row reading as mathematics has no columns to sit between: the caret
      // goes after the whole statement, which is where a new line would go.
      float x = row.x + row.width;
      if (!row.asLatex || prose) {
        const std::string& line = lines_[static_cast<std::size_t>(row.line)];
        const std::size_t  col =
            std::clamp(static_cast<std::size_t>(markCol), row.begin, row.end);
        x = row.x + text::Measure(fonts, row.asLatex ? ProseFace() : Face::Mono,
                                  std::string_view(line).substr(row.begin, col - row.begin),
                                  g.emPx);
      }
      DrawRectangleRec({x, row.y + row.height * 0.08f, std::max(1.0f, g.emPx * 0.07f),
                        row.height * 0.86f},
                       asWords ? kSlideInk : kCaret);
    }
  }

  // A text box being written in has no card to say so, so its rim does.
  if (rimHot_ || dragging_ || selected_ || (asWords && focused_)) {
    DrawRim(g.rect, g.zoom, dragging_ || selected_ || focused_);
  }

}

void CodeBlock::DrawChrome(const Viewport& viewport, FontStore& fonts) const {
  if (absorbed_) return;
  const Geometry& g = Measure(viewport, fonts);
  const Vector2   pointer = hover::Pointer();
  const bool      asWords = IsText() && !showSource_;
  (void)asWords;

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

      // One button showing the justification it is currently set to. Alternate
      // lines are short, which is what makes one side look ragged and the other
      // flush; which side is ragged is the whole of the icon.
      {
        const Rectangle rect = chrome.justify;
        const int       which = static_cast<int>(justify_);
        button(rect, /*on=*/false);
        const float pad = rect.width * 0.22f;
        const float full = rect.width - 2.0f * pad;
        const float thickness = std::max(1.0f, rect.height * 0.075f);
        const float gap = (rect.height - 2.0f * pad) / 3.0f;
        for (int line = 0; line < 4; ++line) {
          const float width = (line % 2 == 0) ? full : full * 0.62f;
          float x = rect.x + pad;
          if (which == 1) x = rect.x + (rect.width - width) * 0.5f;
          if (which == 2) x = rect.x + rect.width - pad - width;
          DrawRectangleRec({x, rect.y + pad + gap * static_cast<float>(line), width, thickness},
                           kChromeText);
        }
      }
    } else if (focused_) {
      const bool overToggle = CheckCollisionPointRec(pointer, chrome.toggle);
      DrawRectangleRounded(chrome.toggle, 0.35f, 8, overToggle ? kChromeHover : kChromeBg);
      // The label names what you will get, not what you have.
      // The button says what pressing it would give you, so it asks what the
      // box is doing rather than what was last chosen: in Auto those differ.
      const char* label = ReadsAsCode() ? "math" : "code";
      const float labelWidth = text::Measure(fonts, Face::MainRegular, label, em);
      text::Draw(fonts, Face::MainRegular, label,
                 {chrome.toggle.x + (chrome.toggle.width - labelWidth) * 0.5f,
                  chrome.toggle.y + chrome.toggle.height * 0.5f + em * 0.34f},
                 em, kChromeText);
    }
  }

}

void CodeBlock::DrawOverlay(const Viewport& viewport, FontStore& fonts) const {
  // A line from the window showing a value to every place that value is
  // written. The box round the name says *that* it is the same thing; the curve
  // says which window it is the same thing as, which matters once two are open.
  //
  // It leaves the *visualization* -- the handle on the number line, the picture
  // on the plot -- and not the pointer. The pointer is only where your hand
  // happens to be; the handle is the thing that is the variable. (A shader's
  // preview is the exception, and it reports the pointer for exactly that
  // reason: there the pointer really is the value.)
  //
  // Drawn in the overlay pass so it runs over every window, including the one
  // being pointed at -- a curve chopped in half by a card would say nothing.
  if (!highlights_.empty() && !absorbed_) {
    const Geometry& g = Measure(viewport, fonts);
    if (g.emPx >= 0.4f) {
      const float thickness = std::max(1.4f, 1.8f * g.zoom);
      // Where the strands of one bundle meet, just outside the card on the side
      // the window is on.
      const float gap = std::max(10.0f, 16.0f * g.zoom);

      for (const ActiveValue& active : highlights_) {
        // Something else is drawing where this value comes from, and that ends
        // at these same names. One cable, not two.
        if (active.wired) continue;
        const Color   tint = VariableColor(active.name);
        const Vector2 from = active.from;

        // Every place this box writes the name. A variable mentioned five times
        // is still one variable, so the five strands are gathered rather than
        // five separate cables leaving the same window.
        std::vector<Rectangle> mentions;
        for (std::size_t i = 0; i < g.rows.size(); ++i) {
          for (const Rectangle& box : MentionRects(i, active.name, g, fonts)) {
            mentions.push_back(box);
          }
        }
        if (mentions.empty()) continue;

        // Which side a wire leaves by is a choice, and a choice re-taken every
        // frame is one that can change its mind halfway through a drag -- which
        // is the flick. So it is taken once, when the value is picked up, and
        // kept until it is let go. Exactly one of the two branches below asks,
        // so one answer is stored per value.
        const auto side = [&](bool fresh) {
          if (!active.held) { heldSide_.erase(active.name); return fresh; }
          return heldSide_.try_emplace(active.name, fresh).first->second;
        };

        // Tangents leave both ends horizontally, which is what makes a curve
        // read as a cable rather than as a stray line.
        const auto cable = [&](Vector2 a, float aReach, Vector2 b, float bReach) {
          DrawSplineSegmentBezierCubic(a, {a.x + aReach, a.y}, {b.x + bReach, b.y}, b, thickness,
                                       Fade(tint, 0.8f));
        };

        if (mentions.size() == 1) {
          const Rectangle box = mentions.front();
          const bool      fromLeft = side(from.x < box.x + box.width * 0.5f);
          const Vector2   target = {fromLeft ? box.x : box.x + box.width,
                                    box.y + box.height * 0.5f};
          const float     reach = std::clamp(std::abs(target.x - from.x) * 0.45f, 24.0f, 190.0f);
          const float     direction = fromLeft ? 1.0f : -1.0f;
          cable(from, reach * direction, target, -reach * direction);
          continue;
        }

        // Which side the bundle leaves by. One answer for the whole bundle, or
        // the strands would set off in opposite directions and never meet.
        const bool  onLeft = side(from.x < g.rect.x + g.rect.width * 0.5f);
        const float outward = onLeft ? -1.0f : 1.0f;

        // The junction sits just clear of the *names*, not of the card. A wide
        // box would otherwise send every strand off to an edge a long way from
        // anything, which looks like the wire is avoiding the thing it points
        // at. Level with the middle of the mentions, as before.
        float meanY = 0.0f;
        float edge = mentions.front().x;
        for (const Rectangle& box : mentions) {
          meanY += box.y + box.height * 0.5f;
          edge = onLeft ? std::min(edge, box.x) : std::max(edge, box.x + box.width);
        }
        meanY /= static_cast<float>(mentions.size());
        const Vector2 junction = {edge + outward * gap, meanY};

        // Out to each mention. They arrive at the junction along the same
        // horizontal tangent, so near it they lie on top of one another and the
        // bundle reads as one wire fanning out.
        for (const Rectangle& box : mentions) {
          const Vector2 target = {onLeft ? box.x : box.x + box.width,
                                  box.y + box.height * 0.5f};
          const float   reach =
              std::clamp(std::abs(target.x - junction.x) * 0.55f, 14.0f, 150.0f);
          cable(junction, -outward * reach, target, outward * reach);
        }

        // And the one wire that actually says which window this is.
        const float reach = std::clamp(std::abs(junction.x - from.x) * 0.45f, 24.0f, 190.0f);
        cable(from, (from.x < junction.x ? 1.0f : -1.0f) * reach, junction, outward * reach);
      }
    }
  }

  // What the driver said, shown against the character it pointed at. The
  // second error comes with it: one mistake usually has another right behind
  // it, and seeing both saves a round trip.
  if (program_.state() != mathprogram::State::Failed || !program_.error().present) return;
  const bool         failed = true;
  const int          errorLine = program_.error().line;
  const int          errorColumn = program_.error().column;
  const std::string& message = program_.error().message;
  const std::string& secondary = program_.error().secondary;

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
  std::vector<std::string> wrapped =
      text::Wrap(fonts, Face::MainRegular, message, messageEm, maxWidth);
  if (wrapped.empty()) return;
  // Wrapped on its own rather than joined on, so the two read as two rather
  // than running into one sentence.
  if (!secondary.empty()) {
    for (const std::string& line :
         text::Wrap(fonts, Face::MainRegular, secondary, messageEm, maxWidth)) {
      wrapped.push_back(line);
    }
  }

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
  const Rectangle rect = {topLeft.x, topLeft.y, layout.width + 2.0f * CardPad() * zoom,
                          layout.height + layout.depth + 2.0f * CardPad() * zoom};

  DrawCard(rect, zoom, kMathBg, Fade(kMathFg, 0.15f), 1.0f);

  const Vector2 origin = {rect.x + CardPad() * zoom, rect.y + CardPad() * zoom + layout.height};
  tex::DrawLayout(layout, fonts, origin, kMathFg);
}

}  // namespace sb
