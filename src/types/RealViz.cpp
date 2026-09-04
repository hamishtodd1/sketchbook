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
#include "theme.h"

namespace sb {
namespace {

// All in world units, so the lines scale with the canvas like everything else.
constexpr float kRowHeight = 46.0f;
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

bool AxisPanel::Shown(const AxisRef& ref) {
  return ref.block == nullptr || ref.block->InScope(ref.name);
}

bool AxisPanel::Shown(const NumberLine& line) {
  for (const AxisRef& ref : line.refs) {
    if (Shown(ref)) return true;
  }
  return false;
}

CodeBlock* AxisPanel::Owner(const NumberLine& line) const {
  if (line.dock != nullptr) return line.dock;
  return line.refs.empty() ? nullptr : line.refs.front().block;
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
    if (line.dock == block) line.dock = line.refs.empty() ? nullptr : line.refs.front().block;
  }
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                              [](const NumberLine& line) { return line.refs.empty(); }),
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
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                              [](const NumberLine& line) { return line.refs.empty(); }),
               lines_.end());
  for (NumberLine& line : lines_) {
    if (line.dock == nullptr) line.dock = line.refs.front().block;
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
        if (!variable.free) continue;
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
                             const Viewport& viewport) {
  if (block == nullptr || name.empty() || drag_ != Drag::None) return false;

  // Off whatever axis it was sharing, and the one it leaves reframes around
  // what is left on it.
  bool found = false;
  for (NumberLine& line : lines_) {
    const std::size_t before = line.refs.size();
    line.refs.erase(std::remove_if(line.refs.begin(), line.refs.end(),
                                   [&](const AxisRef& ref) {
                                     return ref.block == block && ref.name == name;
                                   }),
                    line.refs.end());
    if (line.refs.size() != before) {
      found = true;
      Reframe(line);
    }
  }
  if (!found) return false;
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                              [](const NumberLine& line) { return line.refs.empty(); }),
               lines_.end());

  const float zoom = viewport.Zoom();
  NumberLine  line;
  line.refs.push_back({block, name});
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
void AxisPanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                             FontStore& fonts) {
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    lines_[i].selected = LassoCatches(lasso, RectFor(i, viewport, fonts));
  }
}

void AxisPanel::ClearSelection() {
  for (NumberLine& line : lines_) line.selected = false;
}

bool AxisPanel::AnySelected() const {
  for (const NumberLine& line : lines_) {
    if (line.selected) return true;
  }
  return false;
}

bool AxisPanel::SelectedRimAt(Vector2 point, const Viewport& viewport, FontStore& fonts) const {
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!lines_[i].selected) continue;
    if (PointOnRim(RectFor(i, viewport, fonts), point)) return true;
  }
  return false;
}

void AxisPanel::BeginSelectionDrag() {
  for (NumberLine& line : lines_) line.dragFrom = line.worldPos;
}

void AxisPanel::MoveSelection(Vector2 worldDelta) {
  for (NumberLine& line : lines_) {
    // A docked row has no position of its own: it follows its box, and moves
    // when the box does.
    if (!line.selected || line.docked) continue;
    line.worldPos = {line.dragFrom.x + worldDelta.x, line.dragFrom.y + worldDelta.y};
  }
}

void AxisPanel::DeleteSelection() {
  for (NumberLine& line : lines_) {
    if (!line.selected) continue;
    // The same bargain as closing one by itself: a knob thrown away leaves the
    // value it was choosing behind, written into the code.
    for (const AxisRef& ref : line.refs) {
      if (ref.block == nullptr) continue;
      ref.block->BakeVariable(ref.name);
      ref.block->Hide(ref.name);
    }
  }
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                              [](const NumberLine& line) { return line.selected; }),
               lines_.end());
  drag_ = Drag::None;
}

void AxisPanel::DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const {
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!lines_[i].selected) continue;
    DrawRim(RectFor(i, viewport, fonts), viewport.Zoom(), true);
  }
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

void AxisPanel::ActiveVariables(const Viewport& viewport, FontStore& fonts,
                                std::vector<ActiveValue>* out) const {
  if (out == nullptr) return;

  // Dragging a handle is about exactly one variable, even on a shared axis.
  if (drag_ == Drag::Handle && dragRef_.block != nullptr && dragLine_ < lines_.size()) {
    out->push_back({dragRef_.block, dragRef_.name, MarkerAt(dragLine_, dragRef_, viewport, fonts)});
    return;
  }
  if (drag_ != Drag::None && dragLine_ < lines_.size()) {
    for (const AxisRef& ref : lines_[dragLine_].refs) {
      if (!Shown(ref)) continue;
      out->push_back({ref.block, ref.name, MarkerAt(dragLine_, ref, viewport, fonts)});
    }
    return;
  }

  const Vector2 pointer = GetMousePosition();
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    if (!Shown(lines_[i])) continue;
    const Rectangle row = RectFor(i, viewport, fonts);
    if (!CheckCollisionPointRec(pointer, row) && !PointOnRim(row, pointer)) continue;
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
    return OverMarker(i, pointer, viewport, fonts) ? kNoRow : i;
  }
  return kNoRow;
}

CodeBlock* AxisPanel::InsertTarget(const Viewport& viewport, FontStore& fonts) const {
  if (drag_ != Drag::None) return nullptr;
  const std::size_t index = OfferedRow(GetMousePosition(), viewport, fonts);
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
  const Vector2 pointer = GetMousePosition();

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
    if (!IsMouseButtonDown(button)) {
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

    // A handle within grabbing distance wins over both the rim and the scroll --
    // and it is the same question the pointer was already answering by blinking
    // that handle, so the press keeps the promise the hover made.
    const AxisRef* best = GrabbableAt(i, pointer, viewport, fonts);
    if (best != nullptr) {
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
  const Vector2     pointer = GetMousePosition();
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
    if (carried && OverDropTarget(GetMousePosition())) continue;
    if (line.docked && line.dock != nullptr && line.dock->Absorbed()) continue;
    // Nothing on it is being talked about, so there is nothing for it to say.
    if (!Shown(line)) continue;

    const Rectangle row = RectFor(i, viewport, fonts);
    const Rectangle area = AxisArea(row);
    const float scale = row.height / kRowHeight;

    DrawRectangleRounded(row, 0.28f, 8, kAxisBg);
    DrawRectangleRoundedLinesEx(row, 0.28f, 8, 1.0f, kAxisBorder);

    if (carried || PointOnRim(row, GetMousePosition())) DrawRim(row, zoom, carried);


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
        // geometry is this value's own, laid down by the viz's lines inside the
        // program that has it; the example mesh stands in until that has run.
        const std::vector<float>& tris =
            variable->mesh.empty() && realViz->mesh != nullptr ? *realViz->mesh : variable->mesh;
        const std::vector<float>& segs = variable->lines.empty() && realViz->lines != nullptr
                                             ? *realViz->lines
                                             : variable->lines;
        // The rectangle's middle is the axis line, so a viz that maps a value
        // straight through lands on it.
        const Rectangle box = {area.x, axisY - row.height * 0.5f, area.width, row.height};
        realViz->Draw(tris, segs, variable->value, box,
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
  }
}

}  // namespace sb
