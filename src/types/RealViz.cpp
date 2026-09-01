#include "types/RealViz.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "grid.h"
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

      bool present = false;
      for (const NumberLine& line : lines_) {
        for (const AxisRef& ref : line.refs) {
          if (ref.block == block.get() && ref.name == variable.name) { present = true; break; }
        }
        if (present) break;
      }
      if (present) continue;

      NumberLine line;
      line.refs.push_back({block.get(), variable.name});
      line.dock = block.get();
      line.centre = 0.0f;
      line.span = 4.0f;
      lines_.push_back(line);
    }
  }
  return lines_.size() != before;
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

void AxisPanel::ActiveVariables(const Viewport& viewport, FontStore& fonts,
                                std::vector<std::pair<CodeBlock*, std::string>>* out) const {
  if (out == nullptr) return;

  // Dragging a handle is about exactly one variable, even on a shared axis.
  if (drag_ == Drag::Handle && dragRef_.block != nullptr) {
    out->emplace_back(dragRef_.block, dragRef_.name);
    return;
  }
  if (drag_ != Drag::None && dragLine_ < lines_.size()) {
    for (const AxisRef& ref : lines_[dragLine_].refs) out->emplace_back(ref.block, ref.name);
    return;
  }

  const Vector2 pointer = GetMousePosition();
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    const Rectangle row = RectFor(i, viewport, fonts);
    if (!CheckCollisionPointRec(pointer, row) && !PointOnRim(row, pointer)) continue;
    for (const AxisRef& ref : lines_[i].refs) out->emplace_back(ref.block, ref.name);
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

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
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

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return true;  // hovering claims it

    const Rectangle area = AxisArea(row);
    const float scale = row.height / kRowHeight;

    // A handle within grabbing distance wins over both the rim and panning.
    float bestDistance = 18.0f * scale;
    const AxisRef* best = nullptr;
    for (const AxisRef& ref : line.refs) {
      const mathprogram::Variable* variable = Resolve(ref);
      if (variable == nullptr || !variable->free) continue;
      const float x = area.x + area.width * (0.5f + (variable->value - line.centre) / line.span);
      const float distance = std::abs(pointer.x - x);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = &ref;
      }
    }
    if (best != nullptr) {
      drag_ = Drag::Handle;
      dragLine_ = i;
      dragRef_ = *best;
      dragMoved_ = false;
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

    drag_ = Drag::Pan;
    dragLine_ = i;
    dragMoved_ = false;
    return true;
  }

  (void)blocks;
  return false;
}

void AxisPanel::Draw(const Viewport& viewport, FontStore& fonts) const {
  const float zoom = viewport.Zoom();
  if (zoom < 0.02f) return;

  for (std::size_t i = 0; i < lines_.size(); ++i) {
    const NumberLine& line = lines_[i];
    const Rectangle row = RectFor(i, viewport, fonts);
    const Rectangle area = AxisArea(row);
    const float scale = row.height / kRowHeight;

    DrawRectangleRounded(row, 0.28f, 8, kAxisBg);
    DrawRectangleRoundedLinesEx(row, 0.28f, 8, 1.0f, kAxisBorder);

    const bool carried = (drag_ == Drag::Move && dragMoved_ && i == dragLine_);
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

    // One marker per variable sharing this axis.
    for (const AxisRef& ref : line.refs) {
      const mathprogram::Variable* variable = Resolve(ref);
      if (variable == nullptr) continue;

      const float x = area.x + area.width * (0.5f + (variable->value - line.centre) / line.span);
      const bool offLeft = x < area.x;
      const bool offRight = x > area.x + area.width;
      const float clamped = std::clamp(x, area.x, area.x + area.width);
      // Colour says *which* variable; the shape below says whether it is yours
      // to drag. They are different questions and want different channels.
      const Color tint = ref.block->VariableColor(ref.name);

      if (offLeft || offRight) {
        // Out of view: point the way rather than pretending it is at the edge.
        const float direction = offLeft ? -1.0f : 1.0f;
        DrawTriangle({clamped + direction * 7.0f * scale, axisY},
                     {clamped, axisY - 5.0f * scale},
                     {clamped, axisY + 5.0f * scale}, Fade(tint, 0.55f));
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
