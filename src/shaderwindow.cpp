#include "shaderwindow.h"

#include <algorithm>
#include <cmath>

#include <algorithm>

#include <cstdio>

#include "carry.h"
#include "grid.h"
#include "rlgl.h"
#include "screen.h"
#include "selection.h"
#include "hover.h"
#include "tex/tex.h"
#include "text.h"
#include "theme.h"
#include "windowname.h"

namespace sb {
namespace {

// How far outside the argument's viz the pointer may be and still take hold of
// it, by its `grab`, in pixels at a zoom of one.
constexpr float kPointReach = 4.0f;
// The largest a picture's texture is made: past this a window is mostly off
// screen anyway.
constexpr int kMaxPicture = 4096;

// A view's plane as its viz is told it: value units into the window's own clip
// space, with the pointer as a ray straight down into it.
VizFrame PlaneFrame(const ShaderView& view, Rectangle rect, Vector2 pointer, float zoom) {
  const float span = std::max(view.span, 1e-4f);
  const float half = span * 0.5f;
  VizFrame    frame;
  frame.viewProj = Matrix{};
  frame.viewProj.m0 = 2.0f / span;
  frame.viewProj.m12 = -2.0f * view.centre.x / span;
  frame.viewProj.m5 = 2.0f / span;
  frame.viewProj.m13 = -2.0f * view.centre.y / span;
  frame.viewProj.m10 = 1.0f;
  frame.viewProj.m15 = 1.0f;
  frame.size = {rect.width, rect.height};
  frame.pointer = {pointer.x - rect.x, pointer.y - rect.y};
  frame.rayOrigin = {view.centre.x - half + (pointer.x - rect.x) / std::max(rect.width, 1.0f) * span,
                     view.centre.y + half - (pointer.y - rect.y) / std::max(rect.height, 1.0f) * span,
                     1.0f};
  frame.rayDir = {0.0f, 0.0f, -1.0f};
  frame.eye = {frame.rayOrigin.x, frame.rayOrigin.y, 1.0f};
  frame.pixel = zoom * span / std::max(rect.width, 1.0f);
  return frame;
}

std::string Number(float value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.4g", static_cast<double>(value));
  return buffer;
}

}  // namespace

const mathprogram::Variable* ShaderPanel::PointOf(const ShaderView& view) const {
  if (view.block == nullptr) return nullptr;
  const int which = view.block->FlatPictureFunction();
  const std::vector<mathprogram::Function>& functions = view.block->Functions();
  if (which < 0 || which >= static_cast<int>(functions.size())) return nullptr;
  const mathprogram::Function& fn = functions[static_cast<std::size_t>(which)];
  for (const mathprogram::Variable& variable : view.block->Variables()) {
    if (variable.kind == mathprogram::Kind::Point && variable.argument &&
        variable.declaredLine == fn.firstLine && variable.components.size() == 2) {
      return &variable;
    }
  }
  return nullptr;
}

Vector2 ShaderPanel::ScreenOf(const ShaderView& view, Rectangle rect, const float* at) {
  const float span = std::max(view.span, 1e-4f);
  const float half = span * 0.5f;
  return {rect.x + (at[0] - (view.centre.x - half)) / span * rect.width,
          rect.y + ((view.centre.y + half) - at[1]) / span * rect.height};
}

void ShaderPanel::ReleaseGpu() {
  for (auto& [block, pane] : panes_) pane.Release();
  panes_.clear();
  for (auto& [block, picture] : pictures_) {
    if (picture.loaded) UnloadRenderTexture(picture.target);
  }
  pictures_.clear();
}

void ShaderPanel::Forget(const CodeBlock* block) {
  views_.erase(std::remove_if(views_.begin(), views_.end(),
                              [block](const ShaderView& view) { return view.block == block; }),
               views_.end());
  gone_.Drop([block](const ShaderView& view) { return view.block == block; });
  dragging_ = false;
  pointDragging_ = false;
  if (auto pane = panes_.find(block); pane != panes_.end()) {
    pane->second.Release();
    panes_.erase(pane);
  }
  if (auto picture = pictures_.find(block); picture != pictures_.end()) {
    if (picture->second.loaded) UnloadRenderTexture(picture->second.target);
    pictures_.erase(picture);
  }
}

bool ShaderPanel::Sync(const Blocks& blocks) {
  const std::size_t before = views_.size();

  // A box that has stopped being a shader, or whose window has been thrown
  // away, keeps none.
  // What goes is kept, to come back as it was if its box gets it back
  // (remembered.h) -- a build that failed takes every window away for a moment.
  const auto gone = [](const ShaderView& view) {
    return view.block == nullptr || !view.block->HasShader() ||
           !view.block->Draws() ||
           view.block->DrawnHidden(CodeBlock::kPicture);
  };
  for (const ShaderView& view : views_) {
    if (view.block != nullptr && gone(view)) gone_.Keep(view);
  }
  views_.erase(std::remove_if(views_.begin(), views_.end(), gone), views_.end());

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
    const CodeBlock* mine = block.get();
    if (!gone_.Take([mine](const ShaderView& kept) { return kept.block == mine; }, &view)) {
      view.block = block.get();
    }
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
  const float       side = kFlankSide * zoom * view.size;

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
void ShaderPanel::Rects(const Viewport& viewport, FontStore& fonts,
                        std::vector<Rectangle>* out) const {
  AppendRects(views_, out, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

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
  const Vector2 pointer = hover::Pointer();
  for (std::size_t i = 0; i < views_.size(); ++i) {
    const mathprogram::Variable* point = PointOf(views_[i]);
    if (point == nullptr) continue;
    const Rectangle rect = RectFor(i, viewport, fonts);
    const Vector2   dot = ScreenOf(views_[i], rect, point->components.data());
    const bool      held = pointDragging_ && pointView_ == i;
    // The argument under the pointer, by its viz's `grab`, inside the picture: on
    // the rim you are holding the window, not pointing at a value.
    const auto pane = panes_.find(views_[i].block);
    const bool over = pane != panes_.end() &&
                      pane->second.Nearest(kPointReach * viewport.Zoom(),
                                           [](const VizItem&) { return true; }) >= 0;
    if (!held && (!CheckCollisionPointRec(pointer, rect) || !over)) continue;
    const std::vector<mathprogram::Function>& functions = views_[i].block->Functions();
    const int which = views_[i].block->FlatPictureFunction();
    ActiveValue value{views_[i].block, point->name, dot, /*wired=*/false, held};
    value.firstLine = functions[static_cast<std::size_t>(which)].firstLine;
    value.lastLine = functions[static_cast<std::size_t>(which)].lastLine;
    out->push_back(value);
    return;
  }
}

bool ShaderPanel::HandleInput(const Viewport& viewport, FontStore& fonts) {
  if (views_.empty()) return false;
  const Vector2 pointer = GetMousePosition();

  // The argument's dot, held: where the viz's `drag` says the pointer is asking
  // it to be, which arrives a frame after it was asked.
  if (pointDragging_) {
    if (pointView_ < views_.size()) {
      std::vector<float> parts;
      if (auto pane = panes_.find(views_[pointView_].block);
          pane != panes_.end() && pane->second.TakeDrag(&parts) && parts.size() == 2) {
        views_[pointView_].block->SetComponentValues(pointName_, parts);
      }
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) || pointView_ >= views_.size()) {
      pointDragging_ = false;
    }
    return true;
  }

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

    // The wheel over a picture is the picture's own scale. Only inside it: over
    // the rim it is the canvas's.
    if (!onRim) {
      const float wheel = GetMouseWheelMove();
      if (wheel != 0.0f) {
        views_[i].span = std::clamp(views_[i].span * std::pow(0.85f, wheel), 0.01f, 400.0f);
        return true;
      }
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return true;  // hovering claims it
    if (!onRim) {
      // A press on the dot takes hold of the argument; anywhere else in the
      // picture it is only a look.
      auto pane = panes_.find(views_[i].block);
      if (pane != panes_.end() && pane->second.Drags()) {
        const int j = pane->second.Nearest(kPointReach * viewport.Zoom(),
                                           [](const VizItem&) { return true; });
        if (j >= 0) {
          pointDragging_ = true;
          pointView_ = i;
          pointName_ = pane->second.Items()[static_cast<std::size_t>(j)].name;
        }
      }
      return true;
    }

    dragging_ = true;
    dragView_ = i;
    dragMoved_ = false;
    dragGrab_ = {pointer.x - rect.x, pointer.y - rect.y};
    return true;
  }
  return false;
}

void ShaderPanel::Draw(const Viewport& viewport, FontStore& fonts, VizStore& vizes) const {
  // What a picture that has gone was holding on the GPU goes with it.
  const auto present = [&](const CodeBlock* block) {
    return std::any_of(views_.begin(), views_.end(),
                       [&](const ShaderView& v) { return v.block == block; });
  };
  for (auto it = panes_.begin(); it != panes_.end();) {
    if (present(it->first)) { ++it; continue; }
    it->second.Release();
    it = panes_.erase(it);
  }
  for (auto it = pictures_.begin(); it != pictures_.end();) {
    if (present(it->first)) { ++it; continue; }
    if (it->second.loaded) UnloadRenderTexture(it->second.target);
    it = pictures_.erase(it);
  }
  const float zoom = viewport.Zoom();
  if (zoom < 0.02f) return;
  const Vector2 pointer = GetMousePosition();
  // How a point of the plane is drawn is a viz, the canvas's or a built-in.
  const VizGpu* viz = vizes.Find("vec2", "Plane");

  for (std::size_t i = 0; i < views_.size(); ++i) {
    const ShaderView& view = views_[i];
    if (view.block == nullptr || view.block->Absorbed()) continue;
    const bool carried = dragging_ && dragMoved_ && i == dragView_;
    // Absorbed by the drop target: gone from the canvas until you pull off it.
    if (carried && OverDropTarget(pointer)) continue;

    const Rectangle rect = RectFor(i, viewport, fonts);

    // The picture, into a texture of the window's size and then onto the window:
    // a viz drawn on it reads it there.
    const int w = std::clamp(static_cast<int>(std::lround(rect.width)), 1, kMaxPicture);
    const int h = std::clamp(static_cast<int>(std::lround(rect.height)), 1, kMaxPicture);
    Picture&  picture = pictures_[view.block];
    if (picture.loaded && (picture.target.texture.width != w || picture.target.texture.height != h)) {
      UnloadRenderTexture(picture.target);
      picture.loaded = false;
    }
    if (!picture.loaded) {
      picture.target = LoadRenderTexture(w, h);
      picture.loaded = picture.target.id != 0;
    }
    if (picture.loaded) {
      BeginTextureMode(picture.target);
      ClearBackground(BLANK);
      view.block->DrawPictureInto({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)},
                                  view.centre, view.span);
      EndInnerTexture();
      DrawTexturePro(picture.target.texture,
                     {0.0f, 0.0f, static_cast<float>(w), -static_cast<float>(h)}, rect, {0.0f, 0.0f},
                     0.0f, WHITE);
    }

    // The argument, where the function is being read.
    if (const mathprogram::Variable* point = PointOf(view); viz != nullptr && point != nullptr) {
      VizPane& pane = panes_[view.block];
      pane.Collect();
      pane.Begin();
      const Color tint = view.block->VariableColor(point->name);
      pane.Add({view.block, point->name}, tint, point->components.data(), 2);
      const Vector2  aim = carry::Position();
      VizFrame       frame = PlaneFrame(view, rect, aim, zoom);
      if (picture.loaded) {
        frame.picture = picture.target.texture.id;
        frame.pictureSize = {static_cast<float>(w), static_cast<float>(h)};
      }
      pane.Settle(*viz, frame, {view.centre.x, view.centre.y, view.span, rect.x, rect.y, rect.width, rect.height},
                  CheckCollisionPointRec(aim, rect), pointDragging_ && pointView_ == i ? 0 : -1);
      rlDrawRenderBatchActive();
      rlViewport(static_cast<int>(rect.x), ScreenHeight() - static_cast<int>(rect.y + rect.height),
                 static_cast<int>(rect.width), static_cast<int>(rect.height));
      pane.Draw(*viz, frame);
      rlViewport(0, 0, ScreenWidth(), ScreenHeight());

      // Its name and its x and y beside it while it is pointed at or held, on a
      // backing so they read over any picture -- kept inside the window.
      const Vector2 dot = ScreenOf(view, rect, point->components.data());
      const float   em = 13.0f * zoom;
      const bool    pointed = (pointDragging_ && pointView_ == i) ||
                           (CheckCollisionPointRec(aim, rect) &&
                            pane.Nearest(kPointReach * zoom, [](const VizItem&) { return true; }) >= 0);
      if (pointed && em >= 5.0f && CheckCollisionPointRec(dot, rect)) {
        const tex::Layout name = tex::Render(point->latexName, fonts, em, /*display=*/false);
        const std::string xy =
            "x " + Number(point->components[0]) + "   y " + Number(point->components[1]);
        const float gap = 5.0f * zoom;
        const float width = name.width + gap + text::Measure(fonts, Face::MainRegular, xy, em);
        const float pad = 4.0f * zoom;
        const float height = em * 1.35f;
        // Clear of the loupe, which is half this wide either side of the point.
        const float clear = 17.0f * zoom;
        float left = dot.x + clear;
        if (left + width + pad > rect.x + rect.width) left = dot.x - clear - width;
        left = std::max(left, rect.x + pad);
        const float top = std::clamp(dot.y - height * 0.5f, rect.y + pad,
                                     rect.y + rect.height - height - pad);
        DrawRectangleRounded({left - pad, top, width + 2.0f * pad, height}, 0.35f, 6,
                             Fade(kAxisBg, 0.85f));
        const float baseline = top + height * 0.74f;
        tex::DrawLayout(name, fonts, {left, baseline}, tint);
        text::Draw(fonts, Face::MainRegular, xy, {left + name.width + gap, baseline}, em,
                   kAxisValue);
      }
    }
    DrawRectangleLinesEx(rect, std::max(1.0f, 1.0f * zoom), Fade(kCodeFg, 0.25f));
    if (carried || PointOnRim(rect, hover::Pointer())) DrawRim(rect, zoom, carried);
    if (carried || CheckCollisionPointRec(hover::Pointer(), rect) || PointOnRim(rect, hover::Pointer())) {
      windowname::Offer(rect, "Plane");
    }
  }
}

}  // namespace sb
