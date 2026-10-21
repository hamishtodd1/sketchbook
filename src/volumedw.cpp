#include "volumedw.h"

#include <algorithm>
#include <cmath>

#include "dock.h"
#include "grid.h"
#include "selection.h"
#include "hover.h"
#include "theme.h"
#include "windowname.h"

namespace sb {
namespace {

// How near and how far the eye may get, in units of the cube's half-width.
constexpr float kNearest = 1.2f;
constexpr float kFurthest = 18.0f;
// How wide the eye sees, as a tangent. The same number as the shader's, and it
// has to be: the edges are drawn here and the cloud is drawn there, and they
// are the same cube.
constexpr float kWide = 0.422793f;   // tan(0.4)

// The camera the shader builds, built again on this side so that the twelve
// edges land exactly on the cloud they are the edges of.
struct Eye {
  Vector3 at{};
  Vector3 fwd{};
  Vector3 rt{};
  Vector3 up{};
};

Vector3 Cross(Vector3 a, Vector3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Dot(Vector3 a, Vector3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vector3 Unit(Vector3 v) {
  const float n = std::sqrt(Dot(v, v));
  return (n > 1e-6f) ? Vector3{v.x / n, v.y / n, v.z / n} : Vector3{0.0f, 0.0f, 1.0f};
}

Eye EyeFor(const VolumeView& view, float half) {
  const float yaw = view.orbit.x;
  const float pitch = view.orbit.y;
  const Vector3 e = {-std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                     std::cos(yaw) * std::cos(pitch)};
  Eye eye;
  eye.fwd = {-e.x, -e.y, -e.z};
  const Vector3 wup =
      (std::fabs(eye.fwd.y) > 0.999f) ? Vector3{0.0f, 0.0f, 1.0f} : Vector3{0.0f, 1.0f, 0.0f};
  eye.rt = Unit(Cross(eye.fwd, wup));
  eye.up = Cross(eye.rt, eye.fwd);
  const Vector3 hub = {view.centre.x, view.centre.y, 0.0f};
  const float   away = view.orbit.z * half;
  eye.at = {hub.x + e.x * away, hub.y + e.y * away, hub.z + e.z * away};
  return eye;
}

// Where a point of the space lands on screen, and how far in front of the eye
// it is. Behind the eye it has no place on screen at all, which is why the
// depth comes back with it.
Vector2 Project(const Eye& eye, Vector3 p, Rectangle rect, float* depth) {
  const Vector3 v = {p.x - eye.at.x, p.y - eye.at.y, p.z - eye.at.z};
  *depth = Dot(v, eye.fwd);
  const float safe = std::max(*depth, 1e-4f) * kWide;
  const float sx = Dot(v, eye.rt) / safe;
  const float sy = Dot(v, eye.up) / safe;
  return {rect.x + (sx * 0.5f + 0.5f) * rect.width,
          rect.y + (0.5f - sy * 0.5f) * rect.height};
}

// The twelve edges, as pairs of corners of the unit cube.
constexpr int kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                               {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

// The cube the volume is drawn in, as twelve lines.
//
// The walls are where the function stops being drawn, and without them a cloud
// floats in nothing with no way to tell how big it is or which way it has been
// turned. Drawn here rather than marched in the shader because an edge is a
// line and lines are what this side is good at -- and because a ray that has to
// notice it passed near an edge is a much worse way of saying it.
void DrawEdges(const VolumeView& view, Rectangle rect, float zoom) {
  const float half = std::max(view.span, 1e-4f) * 0.5f;
  const Eye   eye = EyeFor(view, half);
  const Vector3 hub = {view.centre.x, view.centre.y, 0.0f};

  Vector3 corner[8];
  for (int i = 0; i < 8; ++i) {
    corner[i] = {hub.x + ((i & 1) ? half : -half), hub.y + ((i & 2) ? half : -half),
                 hub.z + ((i & 4) ? half : -half)};
  }

  const float thickness = std::max(1.0f, 1.1f * zoom);
  for (const auto& edge : kEdges) {
    float   da = 0.0f, db = 0.0f;
    Vector3 a = corner[edge[0]];
    Vector3 b = corner[edge[1]];
    Project(eye, a, rect, &da);
    Project(eye, b, rect, &db);
    // Both ends behind the eye: the edge is not on screen at all. One end
    // behind: bring it up to the eye plane rather than letting the projection
    // fling it across the window.
    constexpr float kNear = 1e-3f;
    if (da < kNear && db < kNear) continue;
    if (da < kNear || db < kNear) {
      const float cut = (kNear - da) / (db - da);
      const Vector3 mid = {a.x + (b.x - a.x) * cut, a.y + (b.y - a.y) * cut,
                           a.z + (b.z - a.z) * cut};
      (da < kNear ? a : b) = mid;
    }
    float   ignored = 0.0f;
    Vector2 pa = Project(eye, a, rect, &ignored);
    Vector2 pb = Project(eye, b, rect, &ignored);
    DrawLineEx(pa, pb, thickness, Fade(BLACK, 0.5f));
  }
}

}  // namespace

bool VolumePanel::Sync(const Blocks& blocks) {
  const std::size_t before = views_.size();

  views_.erase(std::remove_if(views_.begin(), views_.end(),
                              [](const VolumeView& view) {
                                return view.block == nullptr || !view.block->HasShader() ||
                                       !view.block->HasVolume() ||
                                       view.block->DrawnHidden(CodeBlock::kVolume);
                              }),
               views_.end());

  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    if (!block->HasShader() || !block->HasVolume() ||
        block->DrawnHidden(CodeBlock::kVolume)) {
      continue;
    }
    bool present = false;
    for (const VolumeView& view : views_) {
      if (view.block == block.get()) { present = true; break; }
    }
    if (present) continue;

    VolumeView view;
    view.block = block.get();
    views_.push_back(view);
  }
  return views_.size() != before;
}

void VolumePanel::Forget(const CodeBlock* block) {
  views_.erase(std::remove_if(views_.begin(), views_.end(),
                              [block](const VolumeView& view) { return view.block == block; }),
               views_.end());
  dragging_ = false;
  orbiting_ = false;
}

int VolumePanel::FlankCount(const CodeBlock* block) const {
  int many = 0;
  for (const VolumeView& view : views_) {
    if (view.docked && view.block == block) ++many;
  }
  return many;
}

Rectangle VolumePanel::RectFor(std::size_t index, const Viewport& viewport,
                             FontStore& fonts) const {
  const float zoom = viewport.Zoom();
  const VolumeView& view = views_[index];
  const float side = kFlankSide * zoom * view.size;

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

Rectangle VolumePanel::DockedBounds(const CodeBlock* block, const Viewport& viewport,
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

bool VolumePanel::HandleInput(const Viewport& viewport, FontStore& fonts) {
  if (views_.empty()) return false;
  const Vector2 pointer = GetMousePosition();

  if (orbiting_) {
    if (orbitView_ >= views_.size() || IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
      orbiting_ = false;
      return true;
    }
    const Vector2 delta = GetMouseDelta();
    VolumeView&   view = views_[orbitView_];
    // Dragging right turns the right-hand side towards you, the way a hand
    // on a globe does. Left and right used to be the other way about.
    view.orbit.x += delta.x * 0.008f;
    // All the way round, unlike a surface: there is no underneath to a volume,
    // only another side of it.
    view.orbit.y = std::clamp(view.orbit.y + delta.y * 0.008f, -1.5f, 1.5f);
    return true;
  }

  if (dragging_) {
    if (dragView_ >= views_.size()) { dragging_ = false; return false; }
    const Vector2 delta = GetMouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f) dragMoved_ = true;
    dragScreenPos_ = {pointer.x - dragGrab_.x, pointer.y - dragGrab_.y};

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      if (dragMoved_ && OverDropTarget(pointer)) {
        views_[dragView_].block->HideDrawn(CodeBlock::kVolume);
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

    if (!onRim) {
      // The wheel moves the eye rather than the scale. Under a perspective
      // camera those are two different things, and moving is the one that says
      // something: going in takes you *into* the cloud.
      const float wheel = GetMouseWheelMove();
      if (wheel != 0.0f) {
        views_[i].orbit.z =
            std::clamp(views_[i].orbit.z * std::pow(0.85f, wheel), kNearest, kFurthest);
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

void VolumePanel::Draw(const Viewport& viewport, FontStore& fonts) const {
  const float zoom = viewport.Zoom();
  if (zoom < 0.02f) return;
  const Vector2 pointer = GetMousePosition();

  for (std::size_t i = 0; i < views_.size(); ++i) {
    const VolumeView& view = views_[i];
    if (view.block == nullptr || view.block->Absorbed()) continue;
    const bool carried = dragging_ && dragMoved_ && i == dragView_;
    if (carried && OverDropTarget(pointer)) continue;

    const Rectangle rect = RectFor(i, viewport, fonts);
    view.block->DrawVolumeInto(rect, view.centre, view.span, view.orbit);
    BeginScissorMode(static_cast<int>(rect.x), static_cast<int>(rect.y),
                     static_cast<int>(rect.width), static_cast<int>(rect.height));
    DrawEdges(view, rect, zoom);
    EndScissorMode();
    DrawRectangleLinesEx(rect, std::max(1.0f, 1.0f * zoom), Fade(kCodeFg, 0.25f));
    if (carried || PointOnRim(rect, hover::Pointer())) DrawRim(rect, zoom, carried);
    if (carried || CheckCollisionPointRec(hover::Pointer(), rect) || PointOnRim(rect, hover::Pointer())) {
      windowname::Offer(rect, "Volume");
    }
  }
}

// ------------------------------------------------------------------ selection
void VolumePanel::Rects(const Viewport& viewport, FontStore& fonts,
                        std::vector<Rectangle>* out) const {
  AppendRects(views_, out, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void VolumePanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                               FontStore& fonts) {
  SelectCaught(views_, lasso, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void VolumePanel::ClearSelection() { ClearSelected(views_); }

bool VolumePanel::AnySelected() const { return sb::AnySelected(views_); }

bool VolumePanel::SelectedRimAt(Vector2 point, const Viewport& viewport,
                                FontStore& fonts) const {
  return SelectedRim(views_, point, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void VolumePanel::BeginSelectionDrag() { RememberPositions(views_); }

void VolumePanel::MoveSelection(Vector2 worldDelta) { MoveSelected(views_, worldDelta); }

void VolumePanel::DeleteSelection() {
  for (const VolumeView& view : views_) {
    if (view.selected && view.block != nullptr) view.block->HideDrawn(CodeBlock::kVolume);
  }
  EraseSelected(views_);
  dragging_ = false;
}

void VolumePanel::DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const {
  DrawSelectedRims(views_, viewport.Zoom(),
                   [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

}  // namespace sb
