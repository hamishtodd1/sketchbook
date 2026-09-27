#include "types/RealViz.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "grid.h"
#include "selection.h"
#include "types/examples.h"
#include "tex/tex.h"
#include "text.h"
#include "carry.h"
#include "hover.h"
#include "theme.h"

namespace sb {
namespace {

// All in world units, so the lines scale with the canvas like everything else.
constexpr float kRowHeight = 61.0f;
// Enough gap that a docked row's rim never touches the one above it.
constexpr float kRowGap = 12.0f;
constexpr float kFloatWidth = 340.0f;
// Keeps handles clear of the rim band, so grabbing a value never moves the row.
constexpr float kInset = 20.0f;
// How near a marker counts as being on it. It decides two things at once: which
// handle a press grabs, and whether the axis is bare enough here to be offering
// a new value -- which had better be the same distance, or a press could grab
// one thing while the pointer was promising another.
constexpr float kGrabRadius = 18.0f;
constexpr float kLabelEm = 17.0f;
// The play button: how big, and how far below the axis line its middle is.
constexpr float kPlayRadius = 8.0f;
constexpr float kPlayDrop = 16.0f;
// How far a playing knob swings either side of where it started, as a share of
// the axis's span -- so it sweeps half the row, and stays on it.
constexpr float kPlaySwing = 0.25f;
constexpr float kTickEm = 12.0f;

constexpr float kMinSpan = 1e-4f;
constexpr float kMaxSpan = 1e9f;

// How much of the row the values are allowed to fill when an axis reframes
// itself. The rest is the padding that keeps them off the ends.
constexpr float kFramedFill = 0.7f;

// A 1-2-5 ladder, so tick labels stay round numbers at every zoom.
float NiceStep(float span, float targetCount) {
  const float rough = span / std::max(targetCount, 1.0f);
  const float magnitude = std::pow(10.0f, std::floor(std::log10(std::max(rough, 1e-30f))));
  const float normalised = rough / magnitude;
  float step = 1.0f;
  if (normalised > 5.0f) step = 10.0f;
  else if (normalised > 2.0f) step = 5.0f;
  else if (normalised > 1.0f) step = 2.0f;
  return step * magnitude;
}

std::string FormatTick(float value, float step) {
  // Show exactly as many decimals as the step needs, so labels do not wobble.
  int decimals = static_cast<int>(std::ceil(-std::log10(static_cast<double>(step))));
  decimals = std::clamp(decimals, 0, 6);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, static_cast<double>(value));
  std::string out = buffer;
  if (out == "-0") out = "0";
  return out;
}

std::string FormatValue(float value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.4g", static_cast<double>(value));
  return buffer;
}

}  // namespace

// --------------------------------------------------------- example generator
namespace types {

float ExampleReal(std::string_view name) {
  // A tidy value in [-1, 1) with four decimals -- something a person would not
  // mind seeing written into their code.
  const int scaled = static_cast<int>(NameSeed(name) % 20000u) - 10000;
  return static_cast<float>(scaled) / 10000.0f;
}

}  // namespace types

// ------------------------------------------------------------- the number line
const mathprogram::Variable* AxisPanel::Resolve(const AxisRef& ref) const {
  if (ref.block == nullptr) return nullptr;
  return ref.block->FindVariable(ref.name);
}

namespace {

}  // namespace

void AxisPanel::EnsureField() const {
  if (haveField_) return;
  // One white pixel, stretched across the row. Everything about how the field
  // looks is in the box's own shader; this only exists so that the texture
  // coordinate runs 0..1 across the rectangle being painted.
  Image white = GenImageColor(1, 1, WHITE);
  fieldSamples_ = LoadTextureFromImage(white);
  UnloadImage(white);
  haveField_ = true;
}

void AxisPanel::ReleaseGpu() {
  if (!haveField_) return;
  UnloadTexture(fieldSamples_);
  haveField_ = false;
}

void AxisPanel::DrawField(Rectangle area, const NumberLine& line, const AxisRef& ref) const {
  if (ref.block == nullptr || !ref.block->HasFieldShader()) return;
  EnsureField();

  const Vector2 range = {line.centre - line.span * 0.5f, line.centre + line.span * 0.5f};
  if (!ref.block->BeginField(ref.name, /*mode=*/0, range, {0.0f, 0.0f}, 0.0f)) return;

  const Rectangle source = {0.0f, 0.0f, 1.0f, 1.0f};
  DrawTexturePro(fieldSamples_, source, area, {0.0f, 0.0f}, 0.0f, WHITE);
  ref.block->EndField();
}

namespace {

// A real by any spelling. A function's input and output types are written the
// way the user wrote them, and `double` and `RealFunction` both come back to
// one number on an axis.
bool IsRealSpelling(const std::string& type) {
  return mathline::GlslTypeName(type) == "float";
}

// Whether a box has a function that would put samples on a number line: one
// whose input or whose answer is a real. Asked of every function the box
// defines rather than only the one being read, so the row stays put as the
// caret moves.
bool WantsAxisSamples(const CodeBlock& block) {
  for (const std::string& type : block.SampledTypes()) {
    if (IsRealSpelling(type)) return true;
  }
  return false;
}

// How near counts as on a sample.
constexpr float kSampleGrab = 9.0f;

}  // namespace

bool AxisPanel::Shown(const AxisRef& ref) {
  return ref.block == nullptr || ref.block->InScope(ref.name);
}

bool AxisPanel::Shown(const NumberLine& line) {
  for (const AxisRef& ref : line.refs) {
    if (Shown(ref)) return true;
  }
  // A row carrying only functions is still a row worth drawing.
  for (const AxisRef& ref : line.fields) {
    if (Shown(ref)) return true;
  }
  // And so is one carrying nothing but a spread of samples.
  for (const CodeBlock* block : line.samples) {
    if (block != nullptr && block->Samples().Live()) return true;
  }
  return false;
}

CodeBlock* AxisPanel::Owner(const NumberLine& line) const {
  if (line.dock != nullptr) return line.dock;
  if (!line.refs.empty()) return line.refs.front().block;
  if (!line.fields.empty()) return line.fields.front().block;
  return line.samples.empty() ? nullptr : line.samples.front();
}

void AxisPanel::Reframe(NumberLine& line) const {
  bool  any = false;
  float low = 0.0f;
  float high = 0.0f;
  for (const AxisRef& ref : line.refs) {
    const mathprogram::Variable* variable = Resolve(ref);
    if (variable == nullptr || !variable->known) continue;
    if (!any) {
      low = high = variable->value;
      any = true;
      continue;
    }
    low = std::min(low, variable->value);
    high = std::max(high, variable->value);
  }
  if (!any) return;

  line.centre = (low + high) * 0.5f;
  const float wanted = (high - low) / kFramedFill;
  // Values that coincide say nothing about what scale to read them at, so in
  // that one case the axis keeps the scale it already had.
  if (wanted > kMinSpan) line.span = std::min(wanted, kMaxSpan);
}

void AxisPanel::Forget(const CodeBlock* block) {
  for (NumberLine& line : lines_) {
    line.refs.erase(std::remove_if(line.refs.begin(), line.refs.end(),
                                   [block](const AxisRef& ref) { return ref.block == block; }),
                    line.refs.end());
    line.fields.erase(std::remove_if(line.fields.begin(), line.fields.end(),
                                     [block](const AxisRef& ref) { return ref.block == block; }),
                      line.fields.end());
    line.samples.erase(std::remove(line.samples.begin(), line.samples.end(), block),
                       line.samples.end());
    if (line.dock == block) line.dock = line.refs.empty() ? nullptr : line.refs.front().block;
  }
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                              [](const NumberLine& line) {
                                return line.refs.empty() && line.fields.empty() &&
                                       line.samples.empty();
                              }),
               lines_.end());
  drag_ = Drag::None;
  // The handle being dragged named a box that may be the one going away, and so
  // may the value a click was waiting on.
  dragRef_ = {};
  if (pending_.made.block == block || pending_.anchor.block == block) pending_ = {};
}

bool AxisPanel::Sync(const Blocks& blocks) {
  const std::size_t before = lines_.size();
  // Drop refs whose variable no longer exists.
  for (NumberLine& line : lines_) {
    line.refs.erase(std::remove_if(line.refs.begin(), line.refs.end(),
                                   [this](const AxisRef& ref) { return Resolve(ref) == nullptr; }),
                    line.refs.end());
  }
  for (NumberLine& line : lines_) {
    line.fields.erase(
        std::remove_if(line.fields.begin(), line.fields.end(),
                       [this](const AxisRef& ref) { return Resolve(ref) == nullptr; }),
        line.fields.end());
  }
  // A box that has stopped having a sampled function stops needing a row for
  // one. Asked of the box rather than of the caret, so nothing comes and goes
  // as the caret moves.
  for (NumberLine& line : lines_) {
    line.samples.erase(std::remove_if(line.samples.begin(), line.samples.end(),
                                      [&blocks](const CodeBlock* block) {
                                        for (const std::unique_ptr<CodeBlock>& live : blocks) {
                                          if (live.get() == block) return !WantsAxisSamples(*live);
                                        }
                                        return true;
                                      }),
                       line.samples.end());
  }
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                              [](const NumberLine& line) {
                                return line.refs.empty() && line.fields.empty() &&
                                       line.samples.empty();
                              }),
               lines_.end());
  for (NumberLine& line : lines_) {
    if (line.dock != nullptr) continue;
    if (!line.refs.empty()) line.dock = line.refs.front().block;
    else if (!line.fields.empty()) line.dock = line.fields.front().block;
    else line.dock = line.samples.front();
  }

  // Add a line for anything new.
  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    for (const mathprogram::Variable& variable : block->Variables()) {
      // A function is not a point on a line. It gets a plot instead.
      if (variable.kind != mathprogram::Kind::Real) continue;
      if (block->IsHidden(variable.name)) {
        // A line thrown away stays thrown away -- unless the variable has gone
        // back to being free. Closing a knob is what wrote its value into the
        // code; taking that value out again is asking for the knob back.
        // A typed number is a knob too, but closing its window left the number
        // written in the code -- where it still is -- so it stays closed.
        if (!variable.free || variable.written) continue;
        block->Unhide(variable.name);
      }

      bool present = false;
      for (const NumberLine& line : lines_) {
        for (const AxisRef& ref : line.refs) {
          if (ref.block == block.get() && ref.name == variable.name) { present = true; break; }
        }
        if (present) break;
      }
      if (present) continue;

      // It joins the axis its box already has rather than starting one of its
      // own. The values in a box are read together, so they belong on one
      // scale; a variable that wants its own axis is pulled out onto the canvas.
      NumberLine* home = nullptr;
      bool        placed = false;

      // Unless it was made by clicking an axis, in which case it belongs on the
      // axis it was put on, at the scale it was put at -- it is already in view
      // by construction, and reframing would move everything else to make room
      // for a value that needed none.
      if (pending_.live && pending_.made.block == block.get() &&
          pending_.made.name == variable.name) {
        for (NumberLine& line : lines_) {
          for (const AxisRef& ref : line.refs) {
            if (ref.block != pending_.anchor.block || ref.name != pending_.anchor.name) continue;
            home = &line;
            placed = true;
            break;
          }
          if (home != nullptr) break;
        }
        pending_.live = false;
      }

      if (home == nullptr) {
        for (NumberLine& line : lines_) {
          if (line.docked && line.dock == block.get()) {
            home = &line;
            break;
          }
        }
      }
      if (home == nullptr) {
        NumberLine line;
        line.dock = block.get();
        line.centre = 0.0f;
        line.span = 4.0f;
        lines_.push_back(line);
        home = &lines_.back();
      }
      home->refs.push_back({block.get(), variable.name});
      if (!placed) home->framed = false;
    }
  }

  // And the functions, which join the same axis rather than one of their own:
  // the field is *about* x, and this is the row that says what x is.
  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    for (const mathprogram::Variable& variable : block->Variables()) {
      if (variable.kind != mathprogram::Kind::Function) continue;
      if (block->IsHidden(variable.name)) continue;

      bool present = false;
      for (const NumberLine& line : lines_) {
        for (const AxisRef& ref : line.fields) {
          if (ref.block == block.get() && ref.name == variable.name) { present = true; break; }
        }
        if (present) break;
      }
      if (present) continue;

      // A row of its own. Two fields on one row cannot both be read -- the
      // second is simply painted over the first -- so each function starts
      // alone, and dropping one row onto another is how they come to share a
      // scale, exactly as it is for values.
      NumberLine line;
      line.dock = block.get();
      line.centre = 0.0f;
      line.span = 4.0f;
      line.framed = true;   // a field has no position to frame around
      line.fields.push_back({block.get(), variable.name});
      lines_.push_back(line);
    }
  }

  // And a box whose function is fed with reals, or answers with them, needs a
  // row to spread them along. It joins the row its box already has, for the
  // same reason a variable does: the things a box is about are read together.
  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    if (!WantsAxisSamples(*block)) continue;

    bool present = false;
    for (const NumberLine& line : lines_) {
      for (const CodeBlock* held : line.samples) {
        if (held == block.get()) { present = true; break; }
      }
      if (present) break;
    }
    if (present) continue;

    // A row of its own, exactly as a function's field gets one. The box's own
    // values are read against a scale framed around them, and a function's
    // domain is not one of those values -- it is where the function is being
    // asked, which is a different question and wants its own answer.
    NumberLine line;
    line.dock = block.get();
    line.centre = 0.0f;
    line.span = 4.0f;
    line.framed = false;   // framed around the spread, once it arrives
    line.samples.push_back(block.get());
    lines_.push_back(line);
  }

  // A spread frames itself the first time it exists, so all of it is in view.
  for (NumberLine& line : lines_) {
    if (line.framed || line.samples.empty() || !line.refs.empty()) continue;
    bool  any = false;
    float low = 0.0f;
    float high = 0.0f;
    for (const CodeBlock* held : line.samples) {
      if (held == nullptr || !held->Samples().Live()) continue;
      const Sampling& fed = held->Samples();
      for (int end = 0; end < 2; ++end) {
        const bool                output = (end == 1);
        const std::string&        type = output ? fed.outputType : fed.inputType;
        const int                 stride = output ? fed.outStride : fed.inStride;
        const std::vector<float>& run = output ? fed.outputs : fed.inputs;
        if (!IsRealSpelling(type) || stride != 1) continue;
        for (int k = 0; k < fed.count && k < static_cast<int>(run.size()); ++k) {
          const float value = run[static_cast<std::size_t>(k)];
          low = any ? std::min(low, value) : value;
          high = any ? std::max(high, value) : value;
          any = true;
        }
      }
    }
    if (!any) continue;
    line.centre = (low + high) * 0.5f;
    const float wanted = (high - low) / kFramedFill;
    if (wanted > kMinSpan) line.span = std::min(wanted, kMaxSpan);
    line.framed = true;
  }

  // An axis frames itself once, as soon as everything on it has a value: values
  // shown on one scale are only worth sharing it if they are all in view.
  for (NumberLine& line : lines_) {
    if (line.framed || line.refs.empty()) continue;
    bool ready = true;
    for (const AxisRef& ref : line.refs) {
      const mathprogram::Variable* variable = Resolve(ref);
      if (variable == nullptr || !variable->known) {
        ready = false;
        break;
      }
    }
    if (!ready) continue;
    Reframe(line);
    line.framed = true;
  }
  return lines_.size() != before;
}

bool AxisPanel::BeginPullOut(CodeBlock* block, const std::string& name, Vector2 pointer,
                             const Viewport& viewport, bool field) {
  if (block == nullptr || name.empty() || drag_ != Drag::None) return false;

  // Off whatever axis it was sharing, and the one it leaves reframes around
  // what is left on it.
  bool found = false;
  for (NumberLine& line : lines_) {
    std::vector<AxisRef>& from = field ? line.fields : line.refs;
    const std::size_t     before = from.size();
    from.erase(std::remove_if(from.begin(), from.end(),
                              [&](const AxisRef& ref) {
                                return ref.block == block && ref.name == name;
                              }),
               from.end());
    if (from.size() != before) {
      found = true;
      Reframe(line);
    }
  }
  if (!found) return false;
  // Only a row with nothing left on it goes. It used to be any row with no
  // *values*, which took a function's field row away with it whenever a value
  // was pulled out of the code.
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                              [](const NumberLine& line) {
                                return line.refs.empty() && line.fields.empty() &&
                                       line.samples.empty();
                              }),
               lines_.end());

  const float zoom = viewport.Zoom();
  NumberLine  line;
  (field ? line.fields : line.refs).push_back({block, name});
  line.dock = block;
  line.docked = false;
  line.centre = 0.0f;
  line.span = 4.0f;
  lines_.push_back(line);

  // Carried from its middle, so it arrives under the pointer rather than
  // hanging off one corner of it -- and from here it is an ordinary carried
  // row, dropped by the same code that drops any other.
  dragLine_ = lines_.size() - 1;
  dragGrab_ = {kFloatWidth * zoom * 0.5f, kRowHeight * zoom * 0.5f};
  dragScreenPos_ = {pointer.x - dragGrab_.x, pointer.y - dragGrab_.y};
  lines_.back().worldPos = viewport.ToWorld(dragScreenPos_);
  drag_ = Drag::Move;
  dragMoved_ = true;
  return true;
}

Rectangle AxisPanel::RectFor(std::size_t index, const Viewport& viewport,
                             FontStore& fonts) const {
  const float zoom = viewport.Zoom();
  const NumberLine& line = lines_[index];
  const float boxWidth =
      (line.dock != nullptr) ? line.dock->DockWidth(viewport, fonts) : kFloatWidth * zoom;
  const float width = line.docked ? std::max(boxWidth, 200.0f * zoom) : kFloatWidth * zoom;

  // A row being carried follows the pointer but keeps its place in the stack.
  if (drag_ == Drag::Move && dragMoved_ && index == dragLine_) {
    return {dragScreenPos_.x, dragScreenPos_.y, width, kRowHeight * zoom};
  }

  if (!line.docked || line.dock == nullptr) {
    const Vector2 topLeft = viewport.ToScreen(line.worldPos);
    return {topLeft.x, topLeft.y, kFloatWidth * zoom, kRowHeight * zoom};
  }

  // Docked rows stack under their own box, in order.
  int slot = 0;
  for (std::size_t i = 0; i < index; ++i) {
    if (lines_[i].docked && lines_[i].dock == line.dock) ++slot;
  }
  const Vector2 anchor = line.dock->DockAnchor(viewport, fonts);
  const float y = anchor.y + (kRowGap + static_cast<float>(slot) * (kRowHeight + kRowGap)) * zoom;
  return {anchor.x, y, width, kRowHeight * zoom};
}


// ------------------------------------------------------------------ selection
void AxisPanel::Rects(const Viewport& viewport, FontStore& fonts,
                      std::vector<Rectangle>* out) const {
  AppendRects(lines_, out, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void AxisPanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                             FontStore& fonts) {
  SelectCaught(lines_, lasso, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void AxisPanel::ClearSelection() { ClearSelected(lines_); }

bool AxisPanel::AnySelected() const { return sb::AnySelected(lines_); }

bool AxisPanel::SelectedRimAt(Vector2 point, const Viewport& viewport, FontStore& fonts) const {
  return SelectedRim(lines_, point, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void AxisPanel::BeginSelectionDrag() { RememberPositions(lines_); }

void AxisPanel::MoveSelection(Vector2 worldDelta) { MoveSelected(lines_, worldDelta); }

void AxisPanel::DeleteSelection() {
  for (const NumberLine& line : lines_) {
    if (!line.selected) continue;
    // The same bargain as closing one by itself: a knob thrown away leaves the
    // value it was choosing behind, written into the code.
    for (const AxisRef& ref : line.refs) {
      if (ref.block == nullptr) continue;
      ref.block->BakeVariable(ref.name);
      ref.block->Hide(ref.name);
    }
  }
  EraseSelected(lines_);
  drag_ = Drag::None;
}

void AxisPanel::DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const {
  DrawSelectedRims(lines_, viewport.Zoom(),
                   [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

Vector2 AxisPanel::MarkerAt(std::size_t index, const AxisRef& ref, const Viewport& viewport,
                            FontStore& fonts) const {
  const Rectangle row = RectFor(index, viewport, fonts);
  const Rectangle area = AxisArea(row);
  const float     axisY = row.y + row.height * 0.58f;

  const mathprogram::Variable* variable = Resolve(ref);
  if (variable == nullptr) return {row.x + row.width * 0.5f, axisY};
  // The same position the marker is drawn at, clamped to the axis the same way:
  // a value off the end is pointed at from the edge, and so is its wire.
  const float x =
      area.x + area.width * (0.5f + (variable->value - lines_[index].centre) / lines_[index].span);
  return {std::clamp(x, area.x, area.x + area.width), axisY};
}

bool AxisPanel::SampleAt(std::size_t index, const CodeBlock* block, int sample, bool output,
                         const Viewport& viewport, FontStore& fonts, Vector2* out) const {
  if (block == nullptr) return false;
  const Sampling& fed = block->Samples();
  if (!fed.Live() || sample < 0 || sample >= fed.count) return false;

  // Only reals live on an axis. A function whose other end is a point or a
  // multivector has that end drawn in the window that type goes in.
  const std::vector<float>& run = output ? fed.outputs : fed.inputs;
  const std::string&        type = output ? fed.outputType : fed.inputType;
  const int                 stride = output ? fed.outStride : fed.inStride;
  if (!IsRealSpelling(type) || stride != 1) return false;
  if (static_cast<std::size_t>(sample) >= run.size()) return false;

  const NumberLine& line = lines_[index];
  const Rectangle   row = RectFor(index, viewport, fonts);
  const Rectangle   area = AxisArea(row);
  const float       scale = row.height / kRowHeight;
  const float       axisY = row.y + row.height * 0.58f;
  const float       x = area.x + area.width * (0.5f + (run[static_cast<std::size_t>(sample)] -
                                                 line.centre) / line.span);
  if (x < area.x - 1.0f || x > area.x + area.width + 1.0f) return false;
  // Inputs sit above the line and answers below it, so a function of reals to
  // reals reads as a row of arrows rather than as one muddled line of dots.
  *out = {x, axisY + (output ? 1.0f : -1.0f) * 9.0f * scale};
  return true;
}

bool AxisPanel::SampleMarker(const CodeBlock* block, int sample, bool output,
                             const Viewport& viewport, FontStore& fonts, Vector2* out) const {
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!Shown(lines_[i])) continue;
    for (const CodeBlock* held : lines_[i].samples) {
      if (held != block) continue;
      if (SampleAt(i, block, sample, output, viewport, fonts, out)) return true;
    }
  }
  return false;
}

bool AxisPanel::SampleUnder(Vector2 point, const Viewport& viewport, FontStore& fonts,
                            const CodeBlock** block, int* sample) const {
  float best = kSampleGrab * kSampleGrab;
  bool  found = false;
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!Shown(lines_[i])) continue;
    for (const CodeBlock* held : lines_[i].samples) {
      if (held == nullptr) continue;
      const int count = held->Samples().count;
      for (int k = 0; k < count; ++k) {
        for (int end = 0; end < 2; ++end) {
          Vector2 at{};
          if (!SampleAt(i, held, k, end == 1, viewport, fonts, &at)) continue;
          const float dx = at.x - point.x;
          const float dy = at.y - point.y;
          const float away = dx * dx + dy * dy;
          if (away >= best) continue;
          best = away;
          found = true;
          if (block != nullptr) *block = held;
          if (sample != nullptr) *sample = k;
        }
      }
    }
  }
  return found;
}

void AxisPanel::ActiveVariables(const Viewport& viewport, FontStore& fonts,
                                std::vector<ActiveValue>* out) const {
  if (out == nullptr) return;

  // Dragging a handle is about exactly one variable, even on a shared axis.
  if (drag_ == Drag::Handle && dragRef_.block != nullptr && dragLine_ < lines_.size()) {
    out->push_back({dragRef_.block, dragRef_.name, MarkerAt(dragLine_, dragRef_, viewport, fonts),
                    /*wired=*/false, /*held=*/true});
    return;
  }
  if (drag_ != Drag::None && dragLine_ < lines_.size()) {
    for (const AxisRef& ref : lines_[dragLine_].refs) {
      if (!Shown(ref)) continue;
      out->push_back({ref.block, ref.name, MarkerAt(dragLine_, ref, viewport, fonts),
                      /*wired=*/false, /*held=*/true});
    }
    return;
  }

  const Vector2 pointer = hover::Pointer();
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!Shown(lines_[i])) continue;
    const Rectangle row = RectFor(i, viewport, fonts);
    if (!CheckCollisionPointRec(pointer, row) && !PointOnRim(row, pointer)) continue;
    if (const AxisRef* play = PlayButtonAt(i, pointer, viewport, fonts); play != nullptr) {
      out->push_back({play->block, play->name, MarkerAt(i, *play, viewport, fonts)});
      return;
    }
    // The marker the pointer is on -- free or derived. A press only takes hold
    // of a free one, but pointing at a derived value still has something to
    // say: which thing in the code it is, and where that is written.
    if (const AxisRef* at = MarkerNear(i, pointer, viewport, fonts); at != nullptr) {
      out->push_back({at->block, at->name, MarkerAt(i, *at, viewport, fonts)});
    }
    return;
  }
}

Rectangle AxisPanel::DockedBounds(const CodeBlock* block, const Viewport& viewport,
                                  FontStore& fonts) const {
  Rectangle bounds{0.0f, 0.0f, 0.0f, 0.0f};
  bool      any = false;
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!lines_[i].docked || lines_[i].dock != block) continue;
    // A row being carried is under the pointer, not under its box.
    if (drag_ == Drag::Move && dragMoved_ && i == dragLine_) continue;

    const Rectangle row = RectFor(i, viewport, fonts);
    if (!any) {
      bounds = row;
      any = true;
      continue;
    }
    const float right = std::max(bounds.x + bounds.width, row.x + row.width);
    const float bottom = std::max(bounds.y + bounds.height, row.y + row.height);
    bounds.x = std::min(bounds.x, row.x);
    bounds.y = std::min(bounds.y, row.y);
    bounds.width = right - bounds.x;
    bounds.height = bottom - bounds.y;
  }
  return bounds;
}

bool AxisPanel::OverMarker(std::size_t index, Vector2 pointer, const Viewport& viewport,
                           FontStore& fonts) const {
  const Rectangle   row = RectFor(index, viewport, fonts);
  const Rectangle   area = AxisArea(row);
  const float       scale = row.height / kRowHeight;
  const NumberLine& line = lines_[index];
  for (const AxisRef& ref : line.refs) {
    const mathprogram::Variable* variable = Resolve(ref);
    if (variable == nullptr || !Shown(ref)) continue;
    const float x = area.x + area.width * (0.5f + (variable->value - line.centre) / line.span);
    // Clamped the way the marker itself is, so one pointing in off the end is
    // still something the pointer can be over.
    const float at = std::clamp(x, area.x, area.x + area.width);
    if (std::abs(pointer.x - at) < kGrabRadius * scale) return true;
  }
  return false;
}

// One search, asked two ways: `freeOnly` is the difference between "what would
// a press take hold of" and "what is the pointer on".
static const AxisRef* NearestOn(const NumberLine& line, Rectangle area, float scale,
                                float pointerX, bool freeOnly,
                                const AxisPanel& panel);

const AxisRef* AxisPanel::MarkerNear(std::size_t index, Vector2 pointer,
                                     const Viewport& viewport, FontStore& fonts) const {
  if (index >= lines_.size()) return nullptr;
  const Rectangle row = RectFor(index, viewport, fonts);
  if (!CheckCollisionPointRec(pointer, row) && !PointOnRim(row, pointer)) return nullptr;
  return NearestOn(lines_[index], AxisArea(row), row.height / kRowHeight, pointer.x,
                   /*freeOnly=*/false, *this);
}

bool AxisPanel::PlayButton(std::size_t index, const AxisRef& ref, Vector2 pointer,
                           const Viewport& viewport, FontStore& fonts, Vector2* centre,
                           float* radius) const {
  if (index >= lines_.size() || ref.block == nullptr) return false;
  const NumberLine&            line = lines_[index];
  const mathprogram::Variable* variable = Resolve(ref);
  if (variable == nullptr || !variable->free || !Shown(ref)) return false;
  const Rectangle row = RectFor(index, viewport, fonts);
  if (!CheckCollisionPointRec(pointer, row)) return false;
  const float scale = row.height / kRowHeight;
  // Too small on screen to press is not offered at all.
  if (kPlayRadius * scale < 4.0f) return false;

  const bool playing = ref.block->Playing(ref.name);
  if (!playing && GrabbableAt(index, pointer, viewport, fonts) != &ref) return false;

  const Rectangle area = AxisArea(row);
  const float     at = ref.block->KnobValue(*variable);
  const float     x = area.x + area.width * (0.5f + (at - line.centre) / line.span);
  if (x < area.x || x > area.x + area.width) return false;
  *centre = {x, row.y + row.height * 0.58f + kPlayDrop * scale};
  *radius = kPlayRadius * scale;
  return true;
}

const AxisRef* AxisPanel::PlayButtonAt(std::size_t index, Vector2 pointer,
                                       const Viewport& viewport, FontStore& fonts) const {
  if (index >= lines_.size()) return nullptr;
  for (const AxisRef& ref : lines_[index].refs) {
    Vector2 centre{};
    float   radius = 0.0f;
    if (!PlayButton(index, ref, pointer, viewport, fonts, &centre, &radius)) continue;
    if (CheckCollisionPointCircle(pointer, centre, radius * 1.3f)) return &ref;
  }
  return nullptr;
}

bool AxisPanel::VizUnder(Vector2 point, const Viewport& viewport, FontStore& fonts,
                         AxisRef* out, bool* field, Rectangle* window) const {
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!Shown(lines_[i])) continue;
    const Rectangle row = RectFor(i, viewport, fonts);
    if (!CheckCollisionPointRec(point, row) || PointOnRim(row, point)) continue;
    *window = row;
    if (const AxisRef* at = MarkerNear(i, point, viewport, fonts); at != nullptr) {
      *out = *at;
      *field = false;
      return true;
    }
    // Off every marker, a function's field is still under the point: it has a
    // value all the way across. The one painted last is the one you can see.
    for (auto it = lines_[i].fields.rbegin(); it != lines_[i].fields.rend(); ++it) {
      if (!Shown(*it)) continue;
      *out = *it;
      *field = true;
      return true;
    }
    return false;
  }
  return false;
}

bool AxisPanel::ValueAt(Vector2 point, const Viewport& viewport, FontStore& fonts,
                        AxisRef* out) const {
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!Shown(lines_[i])) continue;
    const AxisRef* found = MarkerNear(i, point, viewport, fonts);
    if (found == nullptr) continue;
    if (out != nullptr) *out = *found;
    return true;
  }
  return false;
}

void AxisPanel::AbandonDrag() {
  if (drag_ == Drag::Handle && dragRef_.block != nullptr) {
    dragRef_.block->SetVariableValue(dragRef_.name, dragValueBefore_);
  }
  drag_ = Drag::None;
  dragRef_ = {};
  dragMoved_ = false;
}

bool AxisPanel::MarkerFor(const CodeBlock* block, const std::string& name,
                          const Viewport& viewport, FontStore& fonts, Vector2* out) const {
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!Shown(lines_[i])) continue;
    for (const AxisRef& ref : lines_[i].refs) {
      if (ref.block != block || ref.name != name || !Shown(ref)) continue;
      if (out != nullptr) *out = MarkerAt(i, ref, viewport, fonts);
      return true;
    }
  }
  return false;
}

const AxisRef* AxisPanel::GrabbableAt(std::size_t index, Vector2 pointer,
                                      const Viewport& viewport, FontStore& fonts) const {
  if (index >= lines_.size()) return nullptr;
  const NumberLine& line = lines_[index];
  const Rectangle   row = RectFor(index, viewport, fonts);
  // The rim is included: a handle within reach wins over it, so that grabbing a
  // value never moves the row by accident.
  if (!CheckCollisionPointRec(pointer, row) && !PointOnRim(row, pointer)) return nullptr;

  return NearestOn(line, AxisArea(row), row.height / kRowHeight, pointer.x, /*freeOnly=*/true,
                   *this);
}

static const AxisRef* NearestOn(const NumberLine& line, Rectangle area, float scale,
                                float pointerX, bool freeOnly, const AxisPanel& panel) {
  float          nearest = kGrabRadius * scale;
  const AxisRef* found = nullptr;
  for (const AxisRef& ref : line.refs) {
    const mathprogram::Variable* variable = panel.Value(ref);
    // A derived marker is read-only, so there is nothing there to take hold of
    // -- but there is still something there to point at.
    if (variable == nullptr || !AxisPanel::Shown(ref)) continue;
    if (freeOnly && !variable->free) continue;
    const float x = area.x + area.width * (0.5f + (variable->value - line.centre) / line.span);
    const float distance = std::abs(pointerX - x);
    if (distance < nearest) {
      nearest = distance;
      found = &ref;
    }
  }
  return found;
}

std::size_t AxisPanel::OfferedRow(Vector2 pointer, const Viewport& viewport,
                                  FontStore& fonts) const {
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!Shown(lines_[i])) continue;
    const Rectangle row = RectFor(i, viewport, fonts);
    // The rim carries the row, so it is not offering anything.
    if (PointOnRim(row, pointer)) return kNoRow;
    if (!CheckCollisionPointRec(pointer, row)) continue;
    if (PlayButtonAt(i, pointer, viewport, fonts) != nullptr) return kNoRow;
    return OverMarker(i, pointer, viewport, fonts) ? kNoRow : i;
  }
  return kNoRow;
}

CodeBlock* AxisPanel::InsertTarget(const Viewport& viewport, FontStore& fonts) const {
  if (drag_ != Drag::None) return nullptr;
  const std::size_t index = OfferedRow(hover::Pointer(), viewport, fonts);
  return index == kNoRow ? nullptr : Owner(lines_[index]);
}

std::string AxisPanel::MakeVariable(std::size_t index, Vector2 pointer, const Viewport& viewport,
                                    FontStore& fonts) {
  if (index >= lines_.size()) return {};
  NumberLine& line = lines_[index];
  CodeBlock*  block = Owner(line);
  if (block == nullptr || line.refs.empty()) return {};

  const Rectangle area = AxisArea(RectFor(index, viewport, fonts));
  const float value = line.centre + line.span * ((pointer.x - area.x) / area.width - 0.5f);

  // Declared, not fixed. A value you have just put down is one you will want to
  // move, and only a *free* variable has a handle to move it by -- a literal in
  // the source could only be dragged by rebuilding the box every frame. So the
  // number goes where every free variable's value goes, into the knob; the
  // source says `float d;` and closing the axis is what would write it down.
  const std::string name = block->FreshName();
  block->InsertDeclaration("float " + name + ";");
  // The rebuild is assembled there and then, so the variable exists from this
  // moment even though nothing has run yet: its marker is on the axis before
  // the pointer has moved, and the drag below has something to hold.
  block->SetVariableValue(name, value);
  pending_ = {{block, name}, line.refs.front(), true};
  return name;
}

Rectangle AxisPanel::AxisArea(Rectangle row) {
  // The strip the values are actually plotted along, past the grip.
  const float scale = row.height / kRowHeight;
  const float left = row.x + kInset * scale;
  const float right = row.x + row.width - kInset * scale;
  return {left, row.y, std::max(right - left, 1.0f), row.height};
}

bool AxisPanel::HandleInput(const Viewport& viewport, FontStore& fonts, const Blocks& blocks) {
  if (lines_.empty()) return false;
  const Vector2 pointer = carry::Position();

  auto valueAt = [](Rectangle area, const NumberLine& line, float x) {
    return line.centre + line.span * ((x - area.x) / area.width - 0.5f);
  };

  // ---- continue whatever drag is already running ---------------------------
  if (drag_ != Drag::None) {
    if (dragLine_ >= lines_.size()) { drag_ = Drag::None; return false; }
    NumberLine& line = lines_[dragLine_];
    const Rectangle row = RectFor(dragLine_, viewport, fonts);
    const Rectangle area = AxisArea(row);
    const Vector2 delta = GetMouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f) dragMoved_ = true;

    switch (drag_) {
      case Drag::Handle:
        if (dragRef_.block != nullptr) {
          dragRef_.block->SetVariableValue(dragRef_.name, valueAt(area, line, pointer.x));
        }
        break;
      case Drag::Pan:
        line.centre -= delta.x / area.width * line.span;
        break;
      case Drag::Move:
        // Only the carried position moves; where the row belongs is decided on
        // release, so the stack stays still while you aim.
        dragScreenPos_ = {pointer.x - dragGrab_.x, pointer.y - dragGrab_.y};
        break;
      default:
        break;
    }

    // Scrolling is the right button's; everything else is the left's. Asking
    // whether the button is still down rather than for the release itself, so a
    // release lost to another window cannot leave a row stuck to the cursor.
    const int button = (drag_ == Drag::Pan) ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT;
    if (!carry::Down(button)) {
      if (drag_ == Drag::Move && dragMoved_) {
        bool merged = false;
        // Dropped on another line? Then they share an axis from now on. This
        // is a way of seeing two values together, not a claim that they are
        // equal, so nothing about either program changes. The two lines may
        // well belong to different boxes.
        for (std::size_t i = 0; i < lines_.size(); ++i) {
          if (i == dragLine_) continue;
          if (!CheckCollisionPointRec(pointer, RectFor(i, viewport, fonts))) continue;

          NumberLine& host = lines_[i];
          for (const AxisRef& ref : lines_[dragLine_].refs) host.refs.push_back(ref);
          for (const AxisRef& ref : lines_[dragLine_].fields) host.fields.push_back(ref);
          // Seeing them together is the whole point of the gesture, so the
          // axis reframes around every value now on it. Done before the erase,
          // while `host` is still a valid reference.
          Reframe(host);
          lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(dragLine_));
          merged = true;
          break;
        }
        if (!merged && OverDropTarget(pointer)) {
          // Thrown away. A number line is a *control*, so whatever it was
          // holding has to go somewhere: the value a knob was choosing is
          // written into the code, where it stays without anything to hold it.
          for (const AxisRef& ref : lines_[dragLine_].refs) {
            if (ref.block == nullptr) continue;
            ref.block->BakeVariable(ref.name);
            ref.block->Hide(ref.name);
          }
          lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(dragLine_));
          merged = true;   // nothing left to place
        }
        if (!merged) {
          // Dropped on empty canvas: it lives there now, on the grid like every
          // other window.
          lines_[dragLine_].docked = false;
          lines_[dragLine_].worldPos =
              SnapToGrid(viewport.ToWorld(dragScreenPos_), viewport.Zoom());
        }
      }
      drag_ = Drag::None;
    }
    return true;
  }

  // ---- start a new interaction --------------------------------------------
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    NumberLine& line = lines_[i];
    if (!Shown(line)) continue;
    const Rectangle row = RectFor(i, viewport, fonts);
    const bool onRim = PointOnRim(row, pointer);
    if (!CheckCollisionPointRec(pointer, row) && !onRim) continue;

    // The wheel belongs to the axis under the pointer, not to the canvas.
    const float wheel = GetMouseWheelMoveV().y;
    if (wheel != 0.0f) {
      const Rectangle area = AxisArea(row);
      const float before = valueAt(area, line, pointer.x);
      line.span = std::clamp(line.span * std::pow(1.15f, -wheel), kMinSpan, kMaxSpan);
      const float after = valueAt(area, line, pointer.x);
      line.centre += before - after;
      return true;
    }

    // The right button scrolls the scale, the way it moves the canvas
    // everywhere else. It is the one thing on the canvas that takes it, which
    // is why the canvas has to be told.
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
      drag_ = Drag::Pan;
      dragLine_ = i;
      dragMoved_ = false;
      return true;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return true;  // hovering claims it

    // The play button, before the handle it stands under.
    if (const AxisRef* play = PlayButtonAt(i, pointer, viewport, fonts); play != nullptr) {
      if (play->block->Playing(play->name)) {
        play->block->StopPlaying(play->name);
        // Where it stopped is where it is now, so that is what the text says.
        play->block->CommitValue(play->name);
      } else {
        play->block->StartPlaying(play->name, line.span * kPlaySwing);
      }
      return true;
    }

    // A handle within grabbing distance wins over both the rim and the scroll --
    // and it is the same question the pointer was already answering by blinking
    // that handle, so the press keeps the promise the hover made.
    const AxisRef* best = GrabbableAt(i, pointer, viewport, fonts);
    if (best != nullptr) {
      // Taking hold of a playing knob takes it over.
      if (best->block != nullptr) {
        best->block->StopPlaying(best->name);
        best->block->BeginValueEdit(best->name);
      }
      drag_ = Drag::Handle;
      dragLine_ = i;
      dragRef_ = *best;
      dragMoved_ = false;
      // Remembered so a second finger arriving can put it back: one finger on a
      // handle has not chosen anything yet.
      const mathprogram::Variable* held = Resolve(*best);
      dragValueBefore_ = (held != nullptr) ? held->value : 0.0f;
      return true;
    }

    // The rim around the edge moves the whole row.
    if (PointOnRim(row, pointer)) {
      drag_ = Drag::Move;
      dragLine_ = i;
      dragMoved_ = false;
      dragGrab_ = {pointer.x - row.x, pointer.y - row.y};
      return true;
    }

    // Anywhere else on the axis: a new value, put exactly where you put it.
    // The drag that made it goes straight on into holding it, so making a
    // marker and moving one are one gesture rather than two.
    const std::string made = MakeVariable(i, pointer, viewport, fonts);
    if (!made.empty()) {
      drag_ = Drag::Handle;
      dragLine_ = i;
      dragRef_ = {Owner(lines_[i]), made};
      dragMoved_ = false;
    }
    return true;
  }

  (void)blocks;
  return false;
}

void AxisPanel::Draw(const Viewport& viewport, FontStore& fonts,
                     const VizStore* vizes) const {
  const float zoom = viewport.Zoom();
  if (zoom < 0.02f) return;

  // The row offering to take a value, if any. The box being pointed into shows
  // *where* the declaration would land; this shows *what* value it would have.
  const Vector2     pointer = hover::Pointer();
  const std::size_t offered =
      drag_ == Drag::None ? OfferedRow(pointer, viewport, fonts) : kNoRow;

  // Which values are in use this frame -- the same set that lights up its name
  // in the code and gets a wire. Those are the markers that blink, so the three
  // things saying "this one" all say it about the same one.
  // A real's viz, if the canvas has one written on it. `float` is the name the
  // user writes, so it is the name a viz box has to claim.
  const Viz* realViz = (vizes == nullptr) ? nullptr : vizes->Find("float");

  std::vector<ActiveValue> active;
  ActiveVariables(viewport, fonts, &active);
  const auto inUse = [&active](const AxisRef& ref) {
    for (const ActiveValue& value : active) {
      if (value.block == ref.block && value.name == ref.name) return true;
    }
    return false;
  };

  for (std::size_t i = 0; i < lines_.size(); ++i) {
    const NumberLine& line = lines_[i];
    const bool carried = (drag_ == Drag::Move && dragMoved_ && i == dragLine_);
    // Absorbed by the drop target, or belonging to a box that is: gone from the
    // canvas until you pull off it.
    if (carried && OverDropTarget(carry::Position())) continue;
    if (line.docked && line.dock != nullptr && line.dock->Absorbed()) continue;
    // Nothing on it is being talked about, so there is nothing for it to say.
    if (!Shown(line)) continue;

    const Rectangle row = RectFor(i, viewport, fonts);
    const Rectangle area = AxisArea(row);
    const float scale = row.height / kRowHeight;

    DrawRectangleRounded(row, 0.28f, 8, kAxisBg);
    DrawRectangleRoundedLinesEx(row, 0.28f, 8, 1.0f, kAxisBorder);

    // The fields first: they are what the row is *made of* where they exist,
    // and the axis, its ticks and its markers are read against them.
    for (const AxisRef& ref : line.fields) {
      if (!Shown(ref)) continue;
      DrawField(area, line, ref);
    }

    if (carried || PointOnRim(row, hover::Pointer())) DrawRim(row, zoom, carried);


    const float axisY = row.y + row.height * 0.58f;
    DrawLineEx({area.x, axisY}, {area.x + area.width, axisY}, std::max(1.0f, 1.2f * scale),
               kAxisLine);

    // Ticks and their labels.
    const float step = NiceStep(line.span, 6.0f);
    const float first = std::ceil((line.centre - line.span * 0.5f) / step) * step;
    for (int k = 0; k < 64; ++k) {
      const float value = first + static_cast<float>(k) * step;
      if (value > line.centre + line.span * 0.5f + step * 0.01f) break;
      const float x = area.x + area.width * (0.5f + (value - line.centre) / line.span);
      const bool origin = std::abs(value) < step * 0.01f;
      DrawLineEx({x, axisY - 4.0f * scale}, {x, axisY + 4.0f * scale},
                 std::max(1.0f, 1.0f * scale), origin ? kAxisOrigin : kAxisTick);

      const std::string label = FormatTick(value, step);
      const float labelEm = kTickEm * zoom;
      if (labelEm >= 5.0f) {
        const float width = text::Measure(fonts, Face::MainRegular, label, labelEm);
        text::Draw(fonts, Face::MainRegular, label,
                   {x - width * 0.5f, row.y + row.height - 6.0f * scale}, labelEm,
                   kAxisTickLabel);
      }
    }

    if (offered == i) {
      const float x = std::clamp(pointer.x, area.x, area.x + area.width);
      DrawLineEx({x, axisY - 9.0f * scale}, {x, axisY + 9.0f * scale},
                 std::max(1.0f, 1.5f * scale), Fade(kAccent, 0.55f));
    }

    // One marker per variable sharing this axis.
    for (const AxisRef& ref : line.refs) {
      const mathprogram::Variable* variable = Resolve(ref);
      if (variable == nullptr || !Shown(ref)) continue;

      const float x = area.x + area.width * (0.5f + (variable->value - line.centre) / line.span);
      const bool offLeft = x < area.x;
      const bool offRight = x > area.x + area.width;
      const float clamped = std::clamp(x, area.x, area.x + area.width);
      // Colour says *which* variable; the shape below says whether it is yours
      // to drag; and a blink says *this one, now*. Three questions, three
      // channels.
      const Color tint = Blink(ref.block->VariableColor(ref.name), inUse(ref));

      if (offLeft || offRight) {
        // Out of view: point the way rather than pretending it is at the edge.
        const float direction = offLeft ? -1.0f : 1.0f;
        DrawTriangle({clamped + direction * 7.0f * scale, axisY},
                     {clamped, axisY - 5.0f * scale},
                     {clamped, axisY + 5.0f * scale}, Fade(tint, 0.55f));
      } else if (realViz != nullptr && realViz->Ready()) {
        // The canvas says how a real is drawn, so it is drawn that way. The
        // geometry is laid down for this value by the viz's own compute shader,
        // which runs only when the value has moved.
        //
        // The rectangle's middle is the axis line, so a viz that maps a value
        // straight through lands on it.
        const Rectangle box = {area.x, axisY - row.height * 0.5f, area.width, row.height};
        realViz->Draw(variable->value, box,
                      {line.centre, 0.0f, line.span, row.height / std::max(area.width, 1.0f) *
                                                         line.span},
                      scale, tint);
      } else if (variable->free) {
        DrawCircleV({clamped, axisY}, 7.0f * scale, tint);
        DrawCircleV({clamped, axisY}, 3.0f * scale, kAxisBg);
      } else {
        DrawCircleV({clamped, axisY}, 5.0f * scale, tint);
      }

      // Label the marker with the same glyphs the equation uses.
      const float labelEm = kLabelEm * zoom;
      if (labelEm >= 5.0f && !offLeft && !offRight) {
        const tex::Layout layout =
            tex::Render(variable->latexName, fonts, labelEm, /*display=*/false);
        const std::string value = FormatValue(variable->value);
        const float valueEm = kTickEm * zoom;
        const float valueWidth = text::Measure(fonts, Face::MainRegular, value, valueEm);
        const float total = layout.width + 5.0f * scale + valueWidth;
        const float lowest = row.x + kInset * scale;
        const float highest = std::max(lowest, row.x + row.width - total - kInset * scale);
        const float left = std::clamp(clamped - total * 0.5f, lowest, highest);
        const float baseline = axisY - 11.0f * scale;
        tex::DrawLayout(layout, fonts, {left, baseline}, tint);
        text::Draw(fonts, Face::MainRegular, value,
                   {left + layout.width + 5.0f * scale, baseline}, valueEm,
                   variable->known ? kAxisValue : kAxisPending);
      }
    }

    // The play buttons, over the tick labels they stand on.
    for (const AxisRef& ref : line.refs) {
      Vector2 centre{};
      float   radius = 0.0f;
      if (!PlayButton(i, ref, pointer, viewport, fonts, &centre, &radius)) continue;
      const bool on = CheckCollisionPointCircle(pointer, centre, radius * 1.3f);
      DrawCircleV(centre, radius, on ? kAccentHover : kAccent);
      if (ref.block->Playing(ref.name)) {
        // Pause: two bars.
        const float w = radius * 0.28f;
        const float hgt = radius * 0.95f;
        DrawRectangleRec({centre.x - radius * 0.38f, centre.y - hgt * 0.5f, w, hgt}, RAYWHITE);
        DrawRectangleRec({centre.x + radius * 0.38f - w, centre.y - hgt * 0.5f, w, hgt}, RAYWHITE);
      } else {
        // Play: a triangle pointing right, anticlockwise for raylib.
        DrawTriangle({centre.x - radius * 0.32f, centre.y - radius * 0.5f},
                     {centre.x - radius * 0.32f, centre.y + radius * 0.5f},
                     {centre.x + radius * 0.55f, centre.y}, RAYWHITE);
      }
    }

    // And the spread a function is being fed with. A function has no one place
    // to be, so this is what it looks like instead: its inputs above the line,
    // its answers below it, and one dot lit at each end when the pointer is on
    // a pair. Nothing here is draggable -- a sample is a place the function was
    // asked about, not a value anybody chose.
    for (const CodeBlock* held : line.samples) {
      if (held == nullptr || !held->Samples().Live()) continue;
      const Sampling& fed = held->Samples();
      const Color     tint = held->VariableColor(fed.name);
      const int       lit = held->SampleHover();
      for (int k = 0; k < fed.count; ++k) {
        for (int end = 0; end < 2; ++end) {
          Vector2 at{};
          if (!SampleAt(i, held, k, end == 1, viewport, fonts, &at)) continue;
          const bool on = (k == lit);
          DrawCircleV(at, (on ? 3.8f : 2.6f) * scale, Fade(tint, on ? 1.0f : 0.85f));
        }
      }
    }
  }
}

}  // namespace sb
