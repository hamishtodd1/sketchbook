#include "surfacedw.h"

#include <algorithm>
#include <cmath>

#include "dock.h"
#include "grid.h"
#include "selection.h"
#include "hover.h"
#include "theme.h"

namespace sb {
namespace {


}  // namespace

bool SurfacePanel::Sync(const Blocks& blocks) {
  const std::size_t before = views_.size();

  views_.erase(std::remove_if(views_.begin(), views_.end(),
                              [](const SurfaceView& view) {
                                return view.block == nullptr || !view.block->HasShader() ||
                                       !view.block->HasSurface() ||
                                       view.block->DrawnHidden(CodeBlock::kSurface);
                              }),
               views_.end());

  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    if (!block->HasShader() || !block->HasSurface() ||
        block->DrawnHidden(CodeBlock::kSurface)) {
      continue;
    }
    bool present = false;
    for (const SurfaceView& view : views_) {
      if (view.block == block.get()) { present = true; break; }
    }
    if (present) continue;

    SurfaceView view;
    view.block = block.get();
    views_.push_back(view);
  }
  return views_.size() != before;
}

void SurfacePanel::Forget(const CodeBlock* block) {
  views_.erase(std::remove_if(views_.begin(), views_.end(),
                              [block](const SurfaceView& view) { return view.block == block; }),
               views_.end());
  dragging_ = false;
  orbiting_ = false;
}

int SurfacePanel::FlankCount(const CodeBlock* block) const {
  int many = 0;
  for (const SurfaceView& view : views_) {
    if (view.docked && view.block == block) ++many;
  }
  return many;
}

Rectangle SurfacePanel::RectFor(std::size_t index, const Viewport& viewport,
                             FontStore& fonts) const {
  const float zoom = viewport.Zoom();
  const SurfaceView& view = views_[index];
  const float side = kFlankSide * zoom;

  if (dragging_ && dragMoved_ && index == dragView_) {
    return {dragScreenPos_.x, dragScreenPos_.y, side, side};
  }
  if (!view.docked || view.block == nullptr) {
    const Vector2 topLeft = viewport.ToScreen(view.worldPos);
    return {topLeft.x, topLeft.y, side, side};
  }

  int slot = FlankBase(before_, view.block);
  for (std::size_t i = 0; i < index; ++i) {
    if (views_[i].docked && views_[i].block == view.block) ++slot;
  }
  return FlankRect(view.block->Measure(viewport, fonts).rect, slot, zoom);
}

Rectangle SurfacePanel::DockedBounds(const CodeBlock* block, const Viewport& viewport,
                                     FontStore& fonts) const {
  Rectangle bounds{0.0f, 0.0f, 0.0f, 0.0f};
  bool      any = false;
  for (std::size_t i = 0; i < views_.size(); ++i) {
    if (!views_[i].docked || views_[i].block != block) continue;
    if (dragging_ && dragMoved_ && i == dragView_) continue;
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

bool SurfacePanel::HandleInput(const Viewport& viewport, FontStore& fonts) {
  if (views_.empty()) return false;
  const Vector2 pointer = GetMousePosition();

  // Turning it: the right button, which is what every window with a camera of
  // its own uses.
  if (orbiting_) {
    if (orbitView_ >= views_.size() || IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
      orbiting_ = false;
      return true;
    }
    const Vector2 delta = GetMouseDelta();
    SurfaceView&  view = views_[orbitView_];
    // Dragging right turns the right-hand side towards you, the way a hand
    // on a globe does. Left and right used to be the other way about.
    view.orbit.x += delta.x * 0.008f;
    // Never below the plane: a height field from underneath is a hole, and the
    // flat picture is one end of the range rather than the middle of it.
    view.orbit.y = std::clamp(view.orbit.y + delta.y * 0.008f, 0.12f, 1.5707963f);
    return true;
  }

  if (dragging_) {
    if (dragView_ >= views_.size()) { dragging_ = false; return false; }
    const Vector2 delta = GetMouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f) dragMoved_ = true;
    dragScreenPos_ = {pointer.x - dragGrab_.x, pointer.y - dragGrab_.y};

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      if (dragMoved_ && OverDropTarget(pointer)) {
        // Closing this one costs nothing: it showed what the box makes, and the
        // box goes on making it.
        views_[dragView_].block->HideDrawn(CodeBlock::kSurface);
        views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(dragView_));
      } else if (dragMoved_) {
        views_[dragView_].docked = false;
        views_[dragView_].worldPos =
            SnapToGrid(viewport.ToWorld(dragScreenPos_), viewport.Zoom());
      }
      dragging_ = false;
    }
    return true;
  }

  for (std::size_t i = 0; i < views_.size(); ++i) {
    const Rectangle rect = RectFor(i, viewport, fonts);
    const bool      onRim = PointOnRim(rect, pointer);
    if (!CheckCollisionPointRec(pointer, rect) && !onRim) continue;

    // The wheel over it is its own scale, the way it is over a plot. Only
    // inside: over the rim the wheel resizes the window.
    if (!onRim) {
      const float wheel = GetMouseWheelMove();
      if (wheel != 0.0f) {
        views_[i].span = std::clamp(views_[i].span * std::pow(0.85f, wheel), 0.01f, 400.0f);
        return true;
      }
      if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        orbiting_ = true;
        orbitView_ = i;
        return true;
      }
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return true;   // hovering claims it
    if (!onRim) return true;

    dragging_ = true;
    dragView_ = i;
    dragMoved_ = false;
    dragGrab_ = {pointer.x - rect.x, pointer.y - rect.y};
    return true;
  }
  return false;
}

void SurfacePanel::Draw(const Viewport& viewport, FontStore& fonts) const {
  const float zoom = viewport.Zoom();
  if (zoom < 0.02f) return;
  const Vector2 pointer = GetMousePosition();

  for (std::size_t i = 0; i < views_.size(); ++i) {
    const SurfaceView& view = views_[i];
    if (view.block == nullptr || view.block->Absorbed()) continue;
    const bool carried = dragging_ && dragMoved_ && i == dragView_;
    if (carried && OverDropTarget(pointer)) continue;

    const Rectangle rect = RectFor(i, viewport, fonts);
    view.block->DrawSurfaceInto(rect, view.centre, view.span, view.orbit);
    DrawRectangleLinesEx(rect, std::max(1.0f, 1.0f * zoom), Fade(kCodeFg, 0.25f));
    if (carried || PointOnRim(rect, hover::Pointer())) DrawRim(rect, zoom, carried);
  }
}

// ------------------------------------------------------------------ selection
void SurfacePanel::Rects(const Viewport& viewport, FontStore& fonts,
                         std::vector<Rectangle>* out) const {
  AppendRects(views_, out, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void SurfacePanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                                FontStore& fonts) {
  SelectCaught(views_, lasso, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void SurfacePanel::ClearSelection() { ClearSelected(views_); }

bool SurfacePanel::AnySelected() const { return sb::AnySelected(views_); }

bool SurfacePanel::SelectedRimAt(Vector2 point, const Viewport& viewport,
                                 FontStore& fonts) const {
  return SelectedRim(views_, point, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void SurfacePanel::BeginSelectionDrag() { RememberPositions(views_); }

void SurfacePanel::MoveSelection(Vector2 worldDelta) { MoveSelected(views_, worldDelta); }

void SurfacePanel::DeleteSelection() {
  for (const SurfaceView& view : views_) {
    if (view.selected && view.block != nullptr) view.block->HideDrawn(CodeBlock::kSurface);
  }
  EraseSelected(views_);
  dragging_ = false;
}

void SurfacePanel::DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const {
  DrawSelectedRims(views_, viewport.Zoom(),
                   [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

}  // namespace sb
