#include "viewport.h"

#include <algorithm>
#include <cmath>

#include "theme.h"

namespace sb {
namespace {

Vector2 ScreenCentre() {
  return {static_cast<float>(GetScreenWidth()) * 0.5f,
          static_cast<float>(GetScreenHeight()) * 0.5f};
}

float Distance(Vector2 a, Vector2 b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

void Viewport::Reset(Vector2 worldCentre, float zoom) {
  target_ = worldCentre;
  zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
  mouseDragging_ = false;
  haveTouchBaseline_ = false;
  touchCount_ = 0;
}

Vector2 Viewport::ToScreen(Vector2 world) const {
  const Vector2 c = ScreenCentre();
  return {(world.x - target_.x) * zoom_ + c.x, (world.y - target_.y) * zoom_ + c.y};
}

Vector2 Viewport::ToWorld(Vector2 screen) const {
  const Vector2 c = ScreenCentre();
  return {(screen.x - c.x) / zoom_ + target_.x, (screen.y - c.y) / zoom_ + target_.y};
}

void Viewport::PanByScreenDelta(Vector2 delta) {
  target_.x -= delta.x / zoom_;
  target_.y -= delta.y / zoom_;
}

void Viewport::ZoomAbout(Vector2 anchor, float factor) {
  if (!(factor > 0.0f) || !std::isfinite(factor)) return;
  const Vector2 before = ToWorld(anchor);
  zoom_ = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);
  const Vector2 after = ToWorld(anchor);
  target_.x += before.x - after.x;
  target_.y += before.y - after.y;
}

void Viewport::UpdateTouch() {
  const int count = std::min(touch::Count(), touch::kMaxPoints);
  const touch::Point* points = touch::Points();
  touchCount_ = count;

  if (count == 0) {
    haveTouchBaseline_ = false;
    return;
  }
  touchCooldown_ = 8;
  mouseDragging_ = false;

  // A gesture is only continuous while the same fingers stay down; if the set
  // changes we re-baseline instead of lurching.
  bool sameContacts = haveTouchBaseline_;
  if (sameContacts) {
    for (int i = 0; i < count; ++i) {
      if (baselineIds_[i] != points[i].id) { sameContacts = false; break; }
    }
  }

  // Track the centroid of up to two fingers, plus how far apart they are.
  Vector2 centroid = points[0].position;
  float   spread = 0.0f;
  if (count >= 2) {
    centroid = {(points[0].position.x + points[1].position.x) * 0.5f,
                (points[0].position.y + points[1].position.y) * 0.5f};
    spread = Distance(points[0].position, points[1].position);
  }

  if (sameContacts) {
    PanByScreenDelta({centroid.x - baselineCentroid_.x, centroid.y - baselineCentroid_.y});
    if (count >= 2 && baselineSpread_ > 1.0f && spread > 1.0f) {
      ZoomAbout(centroid, spread / baselineSpread_);
    }
  }

  for (int i = 0; i < count; ++i) baselineIds_[i] = points[i].id;
  baselineCentroid_ = centroid;
  baselineSpread_ = spread;
  haveTouchBaseline_ = true;
}

void Viewport::UpdateMouse(bool wheelToCanvas) {
  if (touchCooldown_ > 0) {
    --touchCooldown_;
    mouseDragging_ = false;
    return;
  }

  // The right button pans, and it pans wherever the pointer happens to be:
  // nothing on the canvas uses it, so it never has to be taken away from a
  // window under the cursor. Middle-drag does the same, for mice that prefer it.
  const bool held =
      IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
  if (!held) {
    mouseDragging_ = false;
  } else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ||
             IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
    mouseDragging_ = true;
  }
  if (mouseDragging_) PanByScreenDelta(GetMouseDelta());

  // Wheel zoom, unless something under the pointer wants the wheel for its own
  // scale. A precision-touchpad pinch arrives here as ctrl+wheel, so it lands on
  // the same path without extra handling.
  const float wheel = GetMouseWheelMoveV().y;
  if (wheelToCanvas && wheel != 0.0f) ZoomAbout(GetMousePosition(), std::pow(1.15f, wheel));
}

void Viewport::Update(bool interactive, bool pointerFree) {
  if (!interactive) {
    mouseDragging_ = false;
    haveTouchBaseline_ = false;
    touchCount_ = touch::Count();
    if (touchCount_ > 0) touchCooldown_ = 8;
    return;
  }

  // A finger belongs to whatever is under it, so the canvas only takes the
  // gesture when nothing else wants it. Fingers also synthesise mouse events,
  // so the cooldown has to keep running even in the frames we ignore them.
  if (pointerFree) {
    UpdateTouch();
  } else {
    haveTouchBaseline_ = false;
    touchCount_ = touch::Count();
    if (touchCount_ > 0) touchCooldown_ = 8;
  }
  UpdateMouse(pointerFree);
}

}  // namespace sb
