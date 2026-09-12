#include "shaderwindow.h"

#include <algorithm>
#include <cmath>

#include <algorithm>

#include "grid.h"
#include "selection.h"
#include "theme.h"

namespace sb {
namespace {

// World units, like every other window.

}  // namespace

void ShaderPanel::Forget(const CodeBlock* block) {
  views_.erase(std::remove_if(views_.begin(), views_.end(),
                              [block](const ShaderView& view) { return view.block == block; }),
               views_.end());
  dragging_ = false;
}

bool ShaderPanel::Sync(const Blocks& blocks) {
  const std::size_t before = views_.size();

  // A box that has stopped being a shader, or whose window has been thrown
  // away, keeps none.
  views_.erase(std::remove_if(views_.begin(), views_.end(),
                              [](const ShaderView& view) {
                                return view.block == nullptr || !view.block->HasShader() ||
                                       !view.block->Draws() ||
                                       view.block->DrawnHidden(CodeBlock::kPicture);
                              }),
               views_.end());

  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    if (!block->HasShader() || !block->Draws() ||
        block->DrawnHidden(CodeBlock::kPicture)) {
      continue;
    }
    bool present = false;
    for (const ShaderView& view : views_) {
      if (view.block == block.get()) { present = true; break; }
    }
    if (present) continue;

    ShaderView view;
    view.block = block.get();
    views_.push_back(view);
  }
  return views_.size() != before;
}

int ShaderPanel::FlankCount(const CodeBlock* block) const {
  int many = 0;
  for (const ShaderView& view : views_) {
    if (view.docked && view.block == block) ++many;
  }
  return many;
}

Rectangle ShaderPanel::RectFor(std::size_t index, const Viewport& viewport,
                               FontStore& fonts) const {
  const float       zoom = viewport.Zoom();
  const ShaderView& view = views_[index];
  const float       side = kFlankSide * zoom;

  if (dragging_ && dragMoved_ && index == dragView_) {
    return {dragScreenPos_.x, dragScreenPos_.y, side, side};
  }
  if (!view.docked || view.block == nullptr) {
    const Vector2 topLeft = viewport.ToScreen(view.worldPos);
    return {topLeft.x, topLeft.y, side, side};
  }

  // Beside its box rather than under it: what a box *makes* sits next to what it
  // says, and the number lines already have the space below.
  int slot = FlankBase(before_, view.block);
  for (std::size_t i = 0; i < index; ++i) {
    if (views_[i].docked && views_[i].block == view.block) ++slot;
  }
  return FlankRect(view.block->Measure(viewport, fonts).rect, slot, zoom);
}

Rectangle ShaderPanel::DockedBounds(const CodeBlock* block, const Viewport& viewport,
                                    FontStore& fonts) const {
  for (std::size_t i = 0; i < views_.size(); ++i) {
    if (views_[i].block != block || !views_[i].docked) continue;
    if (dragging_ && dragMoved_ && i == dragView_) continue;
    return RectFor(i, viewport, fonts);
  }
  return {0.0f, 0.0f, 0.0f, 0.0f};
}


// ------------------------------------------------------------------ selection
void ShaderPanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                               FontStore& fonts) {
  SelectCaught(views_, lasso, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void ShaderPanel::ClearSelection() { ClearSelected(views_); }

bool ShaderPanel::AnySelected() const { return sb::AnySelected(views_); }

bool ShaderPanel::SelectedRimAt(Vector2 point, const Viewport& viewport, FontStore& fonts) const {
  return SelectedRim(views_, point, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void ShaderPanel::BeginSelectionDrag() { RememberPositions(views_); }

void ShaderPanel::MoveSelection(Vector2 worldDelta) { MoveSelected(views_, worldDelta); }

void ShaderPanel::DeleteSelection() {
  for (const ShaderView& view : views_) {
    if (view.selected && view.block != nullptr) view.block->HideDrawn(CodeBlock::kPicture);
  }
  EraseSelected(views_);
  dragging_ = false;
}

void ShaderPanel::DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const {
  for (std::size_t i = 0; i < views_.size(); ++i) {
    if (!views_[i].selected) continue;
    DrawRim(RectFor(i, viewport, fonts), viewport.Zoom(), true);
  }
}

void ShaderPanel::ActiveVariables(const Viewport& viewport, FontStore& fonts,
                                 std::vector<ActiveValue>* out) const {
  if (out == nullptr) return;
  const Vector2 pointer = GetMousePosition();
  for (std::size_t i = 0; i < views_.size(); ++i) {
    if (views_[i].block == nullptr) continue;
    // Inside the picture only: on the rim you are holding the window, not
    // pointing at a value.
    if (!CheckCollisionPointRec(pointer, RectFor(i, viewport, fonts))) continue;
    // The one window whose wire really does leave the pointer, because here the
    // pointer *is* the value: it is the point the shader is being read at.
    out->push_back({views_[i].block, mathprogram::kShaderInput, pointer});
    return;
  }
}

bool ShaderPanel::HandleInput(const Viewport& viewport, FontStore& fonts) {
  if (views_.empty()) return false;
  const Vector2 pointer = GetMousePosition();

  if (dragging_) {
    if (dragView_ >= views_.size()) { dragging_ = false; return false; }
    const Vector2 delta = GetMouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f) dragMoved_ = true;
    dragScreenPos_ = {pointer.x - dragGrab_.x, pointer.y - dragGrab_.y};

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      if (dragMoved_ && OverDropTarget(pointer)) {
        // Closing this one costs nothing: it shows what the box makes, and the
        // box goes on making it.
        views_[dragView_].block->HideDrawn(CodeBlock::kPicture);
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
    const bool onRim = PointOnRim(rect, pointer);
    if (!CheckCollisionPointRec(pointer, rect) && !onRim) continue;

    // The wheel over a picture is the picture's own scale, the way it is over a
    // plot. Only inside it: over the rim the wheel resizes the window.
    if (!onRim) {
      const float wheel = GetMouseWheelMove();
      if (wheel != 0.0f) {
        views_[i].span = std::clamp(views_[i].span * std::pow(0.85f, wheel), 0.01f, 400.0f);
        return true;
      }
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return true;  // hovering claims it
    if (!onRim) return true;

    dragging_ = true;
    dragView_ = i;
    dragMoved_ = false;
    dragGrab_ = {pointer.x - rect.x, pointer.y - rect.y};
    return true;
  }
  return false;
}

void ShaderPanel::Draw(const Viewport& viewport, FontStore& fonts) const {
  const float zoom = viewport.Zoom();
  if (zoom < 0.02f) return;
  const Vector2 pointer = GetMousePosition();

  for (std::size_t i = 0; i < views_.size(); ++i) {
    const ShaderView& view = views_[i];
    if (view.block == nullptr || view.block->Absorbed()) continue;
    const bool carried = dragging_ && dragMoved_ && i == dragView_;
    // Absorbed by the drop target: gone from the canvas until you pull off it.
    if (carried && OverDropTarget(pointer)) continue;

    const Rectangle rect = RectFor(i, viewport, fonts);

    // Inside a fragment shader a variable is a field, not a number, so reading
    // one back needs a point chosen first. Pointing at the picture chooses it;
    // otherwise it is read at the middle.
    Vector2 probe = {0.5f, 0.5f};
    if (CheckCollisionPointRec(pointer, rect)) {
      probe = {(pointer.x - rect.x) / rect.width, (pointer.y - rect.y) / rect.height};
    }
    view.block->SetProbePoint(probe);

    view.block->DrawPictureInto(rect, view.centre, view.span);
    DrawRectangleLinesEx(rect, std::max(1.0f, 1.0f * zoom), Fade(kCodeFg, 0.25f));
    if (carried || PointOnRim(rect, pointer)) DrawRim(rect, zoom, carried);
  }
}

}  // namespace sb
