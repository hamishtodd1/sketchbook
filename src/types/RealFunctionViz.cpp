#include "types/RealFunctionViz.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "grid.h"
#include "rlgl.h"
#include "selection.h"
#include "types/examples.h"
#include "tex/tex.h"
#include "text.h"
#include "theme.h"

namespace sb {
namespace {

// World units, like everything else on the canvas.
constexpr float kPlotSide = 260.0f;
constexpr float kGap = 12.0f;
constexpr float kLabelEm = 15.0f;
constexpr float kTickEm = 11.0f;

constexpr float kMinSpan = 1e-3f;
constexpr float kMaxSpan = 1e6f;

// How much of the window the curves fill after a merge.
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
  int decimals = static_cast<int>(std::ceil(-std::log10(static_cast<double>(step))));
  decimals = std::clamp(decimals, 0, 6);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, static_cast<double>(value));
  std::string out = buffer;
  if (out == "-0") out = "0";
  return out;
}

}  // namespace

// --------------------------------------------------------- example generator
namespace types {

std::string ExampleRealFunction(std::string_view name) {
  // Stirred past the shared seed so that the shape and its coefficient are not
  // picked from the same bits.
  std::uint32_t hash = NameSeed(name);
  auto next = [&hash]() {
    hash ^= hash << 13;
    hash ^= hash >> 17;
    hash ^= hash << 5;
    return hash;
  };
  auto between = [&next](float low, float high) {
    const float t = static_cast<float>(next() % 1000u) / 1000.0f;
    // Three decimals, like the reals: a number a person would not mind seeing
    // written into their code.
    return std::round((low + (high - low) * t) * 1000.0f) / 1000.0f;
  };

  // Four recognisable shapes: a wave, a parabola, a bump, a line.
  char buffer[128];
  switch (next() % 4u) {
    case 0:
      std::snprintf(buffer, sizeof(buffer), "[](float x) { return sin(%gf * x); }",
                    static_cast<double>(between(0.5f, 2.5f)));
      break;
    case 1:
      std::snprintf(buffer, sizeof(buffer), "[](float x) { return %gf * x * x; }",
                    static_cast<double>(between(-1.0f, 1.0f)));
      break;
    case 2:
      std::snprintf(buffer, sizeof(buffer), "[](float x) { return exp(-%gf * x * x); }",
                    static_cast<double>(between(0.4f, 1.5f)));
      break;
    default:
      std::snprintf(buffer, sizeof(buffer), "[](float x) { return %gf * x; }",
                    static_cast<double>(between(-1.5f, 1.5f)));
      break;
  }
  return buffer;
}

}  // namespace types

// -------------------------------------------------------------------- the plot
const mathprogram::Variable* PlotPanel::Resolve(const CurveRef& ref) const {
  if (ref.block == nullptr) return nullptr;
  return ref.block->FindVariable(ref.name);
}

void PlotPanel::Forget(const CodeBlock* block) {
  for (Plot& plot : plots_) {
    plot.refs.erase(std::remove_if(plot.refs.begin(), plot.refs.end(),
                                   [block](const CurveRef& ref) { return ref.block == block; }),
                    plot.refs.end());
    if (plot.dock == block) plot.dock = plot.refs.empty() ? nullptr : plot.refs.front().block;
  }
  plots_.erase(std::remove_if(plots_.begin(), plots_.end(),
                              [](const Plot& plot) { return plot.refs.empty(); }),
               plots_.end());
  drag_ = Drag::None;
}

bool PlotPanel::Sync(const Blocks& blocks) {
  const std::size_t before = plots_.size();

  for (Plot& plot : plots_) {
    plot.refs.erase(std::remove_if(plot.refs.begin(), plot.refs.end(),
                                   [this](const CurveRef& ref) { return Resolve(ref) == nullptr; }),
                    plot.refs.end());
  }
  plots_.erase(std::remove_if(plots_.begin(), plots_.end(),
                              [](const Plot& plot) { return plot.refs.empty(); }),
               plots_.end());
  for (Plot& plot : plots_) {
    if (plot.dock == nullptr) plot.dock = plot.refs.front().block;
  }

  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    for (const mathprogram::Variable& variable : block->Variables()) {
      if (variable.kind != mathprogram::Kind::Function) continue;
      // A plot thrown away stays thrown away.
      if (block->IsHidden(variable.name)) continue;

      bool present = false;
      for (const Plot& plot : plots_) {
        for (const CurveRef& ref : plot.refs) {
          if (ref.block == block.get() && ref.name == variable.name) { present = true; break; }
        }
        if (present) break;
      }
      if (present) continue;

      Plot plot;
      plot.refs.push_back({block.get(), variable.name});
      plot.dock = block.get();
      plots_.push_back(plot);
    }
  }
  return plots_.size() != before;
}

Rectangle PlotPanel::RectFor(std::size_t index, const Viewport& viewport,
                             FontStore& fonts) const {
  const float zoom = viewport.Zoom();
  const Plot& plot = plots_[index];
  // Square, and the same size docked or not. Unlike a number line it does not
  // widen to its box: both axes are drawn at one scale, so a wide window would
  // just be a lot of empty output range with the curve squeezed up the middle.
  const float width = kPlotSide * zoom;

  if (drag_ == Drag::Move && dragMoved_ && index == dragPlot_) {
    return {dragScreenPos_.x, dragScreenPos_.y, width, kPlotSide * zoom};
  }

  if (!plot.docked || plot.dock == nullptr) {
    const Vector2 topLeft = viewport.ToScreen(plot.worldPos);
    return {topLeft.x, topLeft.y, kPlotSide * zoom, kPlotSide * zoom};
  }

  // Under the box, and under whatever number lines that box already has.
  int slot = 0;
  for (std::size_t i = 0; i < index; ++i) {
    if (plots_[i].docked && plots_[i].dock == plot.dock) ++slot;
  }
  const Vector2 anchor = plot.dock->DockAnchor(viewport, fonts);
  float top = anchor.y;
  if (axes_ != nullptr) {
    const Rectangle lines = axes_->DockedBounds(plot.dock, viewport, fonts);
    if (lines.height > 0.0f) top = lines.y + lines.height;
  }
  const float y = top + (kGap + static_cast<float>(slot) * (kPlotSide + kGap)) * zoom;
  return {anchor.x, y, width, kPlotSide * zoom};
}


// ------------------------------------------------------------------ selection
void PlotPanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                             FontStore& fonts) {
  for (std::size_t i = 0; i < plots_.size(); ++i) {
    plots_[i].selected = LassoCatches(lasso, RectFor(i, viewport, fonts));
  }
}

void PlotPanel::ClearSelection() {
  for (Plot& plot : plots_) plot.selected = false;
}

bool PlotPanel::AnySelected() const {
  for (const Plot& plot : plots_) {
    if (plot.selected) return true;
  }
  return false;
}

bool PlotPanel::SelectedRimAt(Vector2 point, const Viewport& viewport, FontStore& fonts) const {
  for (std::size_t i = 0; i < plots_.size(); ++i) {
    if (!plots_[i].selected) continue;
    if (PointOnRim(RectFor(i, viewport, fonts), point)) return true;
  }
  return false;
}

void PlotPanel::BeginSelectionDrag() {
  for (Plot& plot : plots_) plot.dragFrom = plot.worldPos;
}

void PlotPanel::MoveSelection(Vector2 worldDelta) {
  for (Plot& plot : plots_) {
    // A docked row has no position of its own: it follows its box, and moves
    // when the box does.
    if (!plot.selected || plot.docked) continue;
    plot.worldPos = {plot.dragFrom.x + worldDelta.x, plot.dragFrom.y + worldDelta.y};
  }
}

void PlotPanel::DeleteSelection() {
  for (Plot& plot : plots_) {
    if (!plot.selected) continue;
    // A plot only ever showed you something, so there is nothing to save.
    for (const CurveRef& ref : plot.refs) {
      if (ref.block != nullptr) ref.block->Hide(ref.name);
    }
  }
  plots_.erase(std::remove_if(plots_.begin(), plots_.end(),
                              [](const Plot& plot) { return plot.selected; }),
               plots_.end());
  drag_ = Drag::None;
}

void PlotPanel::DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const {
  for (std::size_t i = 0; i < plots_.size(); ++i) {
    if (!plots_[i].selected) continue;
    DrawRim(RectFor(i, viewport, fonts), viewport.Zoom(), true);
  }
}

bool PlotPanel::Shown(const CurveRef& ref) {
  return ref.block == nullptr || ref.block->InScope(ref.name);
}

bool PlotPanel::Shown(const Plot& plot) {
  for (const CurveRef& ref : plot.refs) {
    if (Shown(ref)) return true;
  }
  return false;
}

bool PlotPanel::ValueAt(Vector2 point, const Viewport& viewport, FontStore& fonts,
                        CurveRef* out) const {
  for (std::size_t i = 0; i < plots_.size(); ++i) {
    if (!Shown(plots_[i])) continue;
    if (!CheckCollisionPointRec(point, RectFor(i, viewport, fonts))) continue;
    for (const CurveRef& ref : plots_[i].refs) {
      if (!Shown(ref)) continue;
      if (out != nullptr) *out = ref;
      return true;
    }
  }
  return false;
}

bool PlotPanel::MarkerFor(const CodeBlock* block, const std::string& name,
                          const Viewport& viewport, FontStore& fonts, Vector2* out) const {
  for (std::size_t i = 0; i < plots_.size(); ++i) {
    if (!Shown(plots_[i])) continue;
    for (const CurveRef& ref : plots_[i].refs) {
      if (ref.block != block || ref.name != name || !Shown(ref)) continue;
      // The middle of the plot, as a curve's wire always leaves: a curve is a
      // whole function and has no one place it is.
      const Rectangle rect = RectFor(i, viewport, fonts);
      if (out != nullptr) *out = {rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
      return true;
    }
  }
  return false;
}

void PlotPanel::ActiveVariables(const Viewport& viewport, FontStore& fonts,
                                std::vector<ActiveValue>* out) const {
  if (out == nullptr) return;

  // A wire leaves the middle of the plot rather than any one point on a curve:
  // a curve is a whole function and has no single place it is, and the colour
  // already says which of them is meant.
  const auto middle = [&](std::size_t index) {
    const Rectangle rect = RectFor(index, viewport, fonts);
    return Vector2{rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
  };

  if (drag_ != Drag::None && dragPlot_ < plots_.size()) {
    for (const CurveRef& ref : plots_[dragPlot_].refs) {
      if (!Shown(ref)) continue;
      out->push_back({ref.block, ref.name, middle(dragPlot_)});
    }
    return;
  }

  const Vector2 pointer = GetMousePosition();
  for (std::size_t i = 0; i < plots_.size(); ++i) {
    if (!Shown(plots_[i])) continue;
    const Rectangle rect = RectFor(i, viewport, fonts);
    if (!CheckCollisionPointRec(pointer, rect) && !PointOnRim(rect, pointer)) continue;
    for (const CurveRef& ref : plots_[i].refs) {
      if (!Shown(ref)) continue;
      out->push_back({ref.block, ref.name, middle(i)});
    }
    return;
  }
}

Rectangle PlotPanel::DockedBounds(const CodeBlock* block, const Viewport& viewport,
                                  FontStore& fonts) const {
  Rectangle bounds{0.0f, 0.0f, 0.0f, 0.0f};
  bool      any = false;
  for (std::size_t i = 0; i < plots_.size(); ++i) {
    if (!plots_[i].docked || plots_[i].dock != block) continue;
    if (drag_ == Drag::Move && dragMoved_ && i == dragPlot_) continue;

    const Rectangle rect = RectFor(i, viewport, fonts);
    if (!any) {
      bounds = rect;
      any = true;
      continue;
    }
    const float right = std::max(bounds.x + bounds.width, rect.x + rect.width);
    const float bottom = std::max(bounds.y + bounds.height, rect.y + rect.height);
    bounds.x = std::min(bounds.x, rect.x);
    bounds.y = std::min(bounds.y, rect.y);
    bounds.width = right - bounds.x;
    bounds.height = bottom - bounds.y;
  }
  return bounds;
}

void PlotPanel::Reframe(Plot& plot) const {
  bool  any = false;
  float low = 0.0f;
  float high = 0.0f;
  const float inputLow = plot.centre.y - plot.span * 0.5f;
  const float inputHigh = plot.centre.y + plot.span * 0.5f;

  for (const CurveRef& ref : plot.refs) {
    const mathprogram::Variable* variable = Resolve(ref);
    if (variable == nullptr) continue;
    for (std::size_t i = 0; i < variable->samples.size(); ++i) {
      // Only what is actually on screen: a parabola is unbounded, and framing
      // its far tails would squash everything else to a line.
      const float input = types::realfunction::SampleInput(static_cast<int>(i));
      if (input < inputLow || input > inputHigh) continue;
      const float value = variable->samples[i];
      if (!std::isfinite(value)) continue;
      if (!any) {
        low = high = value;
        any = true;
        continue;
      }
      low = std::min(low, value);
      high = std::max(high, value);
    }
  }
  if (!any) return;

  plot.centre.x = (low + high) * 0.5f;
  // Never zoom in: the input range on show is not the merge's business.
  const float wanted = (high - low) / kFramedFill;
  if (wanted > plot.span) plot.span = std::min(wanted, kMaxSpan);
}

bool PlotPanel::HandleInput(const Viewport& viewport, FontStore& fonts) {
  if (plots_.empty()) return false;
  const Vector2 pointer = GetMousePosition();

  // Where a screen point falls in value space: x is output, y is input.
  auto valueAt = [](Rectangle rect, const Plot& plot, Vector2 point) {
    const float unit = rect.height / std::max(plot.span, kMinSpan);
    return Vector2{plot.centre.x + (point.x - (rect.x + rect.width * 0.5f)) / unit,
                   plot.centre.y - (point.y - (rect.y + rect.height * 0.5f)) / unit};
  };

  if (drag_ != Drag::None) {
    if (dragPlot_ >= plots_.size()) { drag_ = Drag::None; return false; }
    Plot& plot = plots_[dragPlot_];
    const Rectangle rect = RectFor(dragPlot_, viewport, fonts);
    const Vector2 delta = GetMouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f) dragMoved_ = true;

    if (drag_ == Drag::Pan) {
      const float unit = rect.height / std::max(plot.span, kMinSpan);
      plot.centre.x -= delta.x / unit;
      plot.centre.y += delta.y / unit;
    } else if (drag_ == Drag::Move) {
      dragScreenPos_ = {pointer.x - dragGrab_.x, pointer.y - dragGrab_.y};
    }

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      if (drag_ == Drag::Move && dragMoved_) {
        bool merged = false;
        for (std::size_t i = 0; i < plots_.size(); ++i) {
          if (i == dragPlot_) continue;
          if (!CheckCollisionPointRec(pointer, RectFor(i, viewport, fonts))) continue;

          // Two functions against one set of axes. Nothing about either program
          // changes; this is a way of seeing a sum next to its parts.
          Plot& host = plots_[i];
          for (const CurveRef& ref : plots_[dragPlot_].refs) host.refs.push_back(ref);
          Reframe(host);
          plots_.erase(plots_.begin() + static_cast<std::ptrdiff_t>(dragPlot_));
          merged = true;
          break;
        }
        if (!merged && OverDropTarget(pointer)) {
          // A plot only ever showed you something; closing it takes nothing
          // away but the view.
          for (const CurveRef& ref : plots_[dragPlot_].refs) {
            if (ref.block != nullptr) ref.block->Hide(ref.name);
          }
          plots_.erase(plots_.begin() + static_cast<std::ptrdiff_t>(dragPlot_));
          merged = true;
        }
        if (!merged) {
          plots_[dragPlot_].docked = false;
          plots_[dragPlot_].worldPos =
              SnapToGrid(viewport.ToWorld(dragScreenPos_), viewport.Zoom());
        }
      }
      drag_ = Drag::None;
    }
    return true;
  }

  for (std::size_t i = 0; i < plots_.size(); ++i) {
    Plot& plot = plots_[i];
    if (!Shown(plot)) continue;   // not drawn, so not there to be used
    const Rectangle rect = RectFor(i, viewport, fonts);
    const bool onRim = PointOnRim(rect, pointer);
    if (!CheckCollisionPointRec(pointer, rect) && !onRim) continue;

    const float wheel = GetMouseWheelMoveV().y;
    if (wheel != 0.0f) {
      const Vector2 before = valueAt(rect, plot, pointer);
      plot.span = std::clamp(plot.span * std::pow(1.15f, -wheel), kMinSpan, kMaxSpan);
      const Vector2 after = valueAt(rect, plot, pointer);
      plot.centre.x += before.x - after.x;
      plot.centre.y += before.y - after.y;
      return true;
    }

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return true;  // hovering claims it

    if (onRim) {
      drag_ = Drag::Move;
      dragPlot_ = i;
      dragMoved_ = false;
      dragGrab_ = {pointer.x - rect.x, pointer.y - rect.y};
      return true;
    }

    drag_ = Drag::Pan;
    dragPlot_ = i;
    dragMoved_ = false;
    return true;
  }

  return false;
}

void PlotPanel::Draw(const Viewport& viewport, FontStore& fonts, const VizStore* vizes) const {
  const float zoom = viewport.Zoom();
  if (zoom < 0.02f) return;

  // How the canvas says a function is drawn, if it says anything.
  const Viz* curveViz =
      (vizes == nullptr) ? nullptr : vizes->Find(types::realfunction::kName);

  // The curves in use this frame -- the same set that lights up its name in the
  // code and gets a wire. Those are the ones that blink.
  std::vector<ActiveValue> active;
  ActiveVariables(viewport, fonts, &active);
  const auto inUse = [&active](const CurveRef& ref) {
    for (const ActiveValue& value : active) {
      if (value.block == ref.block && value.name == ref.name) return true;
    }
    return false;
  };

  for (std::size_t i = 0; i < plots_.size(); ++i) {
    const Plot& plot = plots_[i];
    const bool carried = (drag_ == Drag::Move && dragMoved_ && i == dragPlot_);
    if (carried && OverDropTarget(GetMousePosition())) continue;
    if (plot.docked && plot.dock != nullptr && plot.dock->Absorbed()) continue;
    // Nothing on it is being talked about, so there is nothing for it to say.
    if (!Shown(plot)) continue;

    const Rectangle rect = RectFor(i, viewport, fonts);
    const float scale = rect.height / kPlotSide;
    const float unit = rect.height / std::max(plot.span, kMinSpan);   // pixels per unit
    const float midX = rect.x + rect.width * 0.5f;
    const float midY = rect.y + rect.height * 0.5f;

    // Output runs right, input runs up. Screen y grows downwards, so the input
    // term is subtracted: that single minus is the whole convention.
    auto toScreen = [&](float output, float input) {
      return Vector2{midX + (output - plot.centre.x) * unit,
                     midY - (input - plot.centre.y) * unit};
    };

    DrawRectangleRounded(rect, 0.06f, 8, kAxisBg);
    DrawRectangleRoundedLinesEx(rect, 0.06f, 8, 1.0f, kAxisBorder);

    if (carried || PointOnRim(rect, GetMousePosition())) DrawRim(rect, zoom, carried);

    // Nothing spills out of the card, so a steep curve does not scribble over
    // the rest of the canvas.
    BeginScissorMode(static_cast<int>(rect.x), static_cast<int>(rect.y),
                     static_cast<int>(rect.width), static_cast<int>(rect.height));

    const float step = NiceStep(plot.span, 6.0f);
    const float thin = std::max(1.0f, 1.0f * scale);

    // Ticks along both axes, at the axis line where there is one and at the
    // edge of the card where the axis has been panned out of view.
    const Vector2 origin = toScreen(0.0f, 0.0f);
    const float axisX = std::clamp(origin.x, rect.x + 1.0f, rect.x + rect.width - 1.0f);
    const float axisY = std::clamp(origin.y, rect.y + 1.0f, rect.y + rect.height - 1.0f);

    const float labelEm = kTickEm * zoom;
    for (int k = -64; k <= 64; ++k) {
      const float value = std::round(plot.centre.x / step) * step + static_cast<float>(k) * step;
      const float x = toScreen(value, 0.0f).x;
      if (x < rect.x || x > rect.x + rect.width) continue;
      if (std::abs(value) < step * 0.01f) continue;
      DrawLineEx({x, axisY - 3.0f * scale}, {x, axisY + 3.0f * scale}, thin, kAxisTick);
      // A label the card would cut in half is worse than no label.
      const bool room = x > rect.x + 14.0f * scale && x < rect.x + rect.width - 14.0f * scale;
      if (labelEm >= 5.0f && room) {
        const std::string label = FormatTick(value, step);
        const float width = text::Measure(fonts, Face::MainRegular, label, labelEm);
        text::Draw(fonts, Face::MainRegular, label, {x - width * 0.5f, axisY + 13.0f * scale},
                   labelEm, kAxisTickLabel);
      }
    }
    for (int k = -64; k <= 64; ++k) {
      const float value = std::round(plot.centre.y / step) * step + static_cast<float>(k) * step;
      const float y = toScreen(0.0f, value).y;
      if (y < rect.y || y > rect.y + rect.height) continue;
      if (std::abs(value) < step * 0.01f) continue;
      DrawLineEx({axisX - 3.0f * scale, y}, {axisX + 3.0f * scale, y}, thin, kAxisTick);
      const bool room = y > rect.y + 14.0f * scale && y < rect.y + rect.height - 6.0f * scale;
      if (labelEm >= 5.0f && room) {
        const std::string label = FormatTick(value, step);
        text::Draw(fonts, Face::MainRegular, label, {axisX + 6.0f * scale, y + labelEm * 0.35f},
                   labelEm, kAxisTickLabel);
      }
    }

    // The two axes themselves.
    DrawLineEx({rect.x, axisY}, {rect.x + rect.width, axisY}, std::max(1.0f, 1.2f * scale),
               kAxisLine);
    DrawLineEx({axisX, rect.y}, {axisX, rect.y + rect.height}, std::max(1.0f, 1.2f * scale),
               kAxisLine);

    // One polyline per function on these axes.
    for (std::size_t c = 0; c < plot.refs.size(); ++c) {
      const mathprogram::Variable* variable = Resolve(plot.refs[c]);
      if (variable == nullptr || !Shown(plot.refs[c])) continue;
      const Color tint =
          Blink(plot.refs[c].block->VariableColor(plot.refs[c].name), inUse(plot.refs[c]));
      const float thickness = std::max(1.4f, 2.0f * scale);

      // The canvas says how a function is drawn, so it is drawn that way. Its
      // lines ran inside the program that owns the function -- the only place
      // one can be sampled -- so what arrives here is already this curve.
      if (curveViz != nullptr && curveViz->Ready() &&
          (!variable->lines.empty() || !variable->mesh.empty())) {
        rlSetLineWidth(thickness);
        curveViz->Draw(variable->mesh, variable->lines, 0.0f, rect,
                       {plot.centre.x, plot.centre.y, plot.span * rect.width / rect.height,
                        plot.span},
                       scale, tint);
        rlSetLineWidth(1.0f);
        continue;
      }
      if (variable->samples.empty()) continue;

      bool    havePrevious = false;
      Vector2 previous{};
      for (std::size_t s = 0; s < variable->samples.size(); ++s) {
        const float value = variable->samples[s];
        if (!std::isfinite(value)) { havePrevious = false; continue; }
        const Vector2 point = toScreen(value, types::realfunction::SampleInput(static_cast<int>(s)));
        if (havePrevious) DrawLineEx(previous, point, thickness, tint);
        previous = point;
        havePrevious = true;
      }
    }

    EndScissorMode();

    // Name each curve, in its own colour, in the corner.
    const float nameEm = kLabelEm * zoom;
    if (nameEm >= 6.0f) {
      float x = rect.x + 10.0f * scale;
      const float baseline = rect.y + 6.0f * scale + nameEm;
      for (std::size_t c = 0; c < plot.refs.size(); ++c) {
        const mathprogram::Variable* variable = Resolve(plot.refs[c]);
        if (variable == nullptr || !Shown(plot.refs[c])) continue;
        const tex::Layout layout =
            tex::Render(variable->latexName, fonts, nameEm, /*display=*/false);
        tex::DrawLayout(
            layout, fonts, {x, baseline},
            Blink(plot.refs[c].block->VariableColor(plot.refs[c].name), inUse(plot.refs[c])));
        x += layout.width + 12.0f * scale;
      }
    }
  }
}

}  // namespace sb
