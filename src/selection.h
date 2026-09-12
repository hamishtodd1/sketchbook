// The lasso, and what it catches.
//
// Left-dragging the background draws a loop; everything whose middle is inside
// it is selected. Selected windows wear their rim, so what is selected is shown
// the same way a window being pointed at is -- there is no second visual
// language for it.
//
// A selection then behaves as one window: it moves together, it can be carried
// onto the drop target together, and Delete throws it all away. Copying takes
// only the *boxes* in it, because a number line without the box whose variable
// it stands for would be a window about nothing.
//
// **The loop closes with a curve, not a chord.** The gap between where the
// pointer is and where the stroke began is bridged by a cubic Bezier, and the
// four points that decide it are the four the stroke itself gives: the last two
// samples say which way it is heading, and the first two say which way it left.
// So the closure continues the gesture instead of cutting across it, and a
// lasso drawn as a spiral or a hook encloses what it looks like it encloses.
//
// Drawing and catching both go through the same closed loop, so what is caught
// is exactly what you watched yourself circle.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include "raylib.h"
#include "theme.h"

namespace sb {

inline constexpr Color kLassoLine  = {  38, 110, 220, 235 };
inline constexpr Color kLassoClose = {  38, 110, 220,  90 };
// Points nearer than this to the last one are not worth remembering.
inline constexpr float kLassoStep = 4.0f;
// How far the closing curve leans along the stroke's direction, as a fraction
// of the gap it is bridging. Proportional, because the samples themselves are
// only a few pixels apart and say nothing about how far there is to go.
inline constexpr float kLassoCloseReach = 0.4f;
// How finely the closing curve is flattened for the hit test.
inline constexpr int kLassoCloseSegments = 16;

// The four control points of the closing curve, from the last sample back to
// the first. Returns false if there is not enough of a stroke to curve.
inline bool LassoClosure(const std::vector<Vector2>& lasso, Vector2 out[4]) {
  if (lasso.size() < 3) return false;

  const Vector2 from = lasso.back();
  const Vector2 to = lasso.front();
  const Vector2 beforeEnd = lasso[lasso.size() - 2];
  const Vector2 afterStart = lasso[1];

  const auto unit = [](Vector2 v) -> Vector2 {
    const float length = std::sqrt(v.x * v.x + v.y * v.y);
    if (!(length > 1e-4f)) return {0.0f, 0.0f};
    return {v.x / length, v.y / length};
  };
  // Where the stroke is going, and where it came from when it set out.
  const Vector2 ahead = unit({from.x - beforeEnd.x, from.y - beforeEnd.y});
  const Vector2 behind = unit({to.x - afterStart.x, to.y - afterStart.y});
  if ((ahead.x == 0.0f && ahead.y == 0.0f) || (behind.x == 0.0f && behind.y == 0.0f)) {
    return false;
  }

  const float reach =
      std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y)) *
      kLassoCloseReach;
  out[0] = from;
  out[1] = {from.x + ahead.x * reach, from.y + ahead.y * reach};
  out[2] = {to.x + behind.x * reach, to.y + behind.y * reach};
  out[3] = to;
  return true;
}

inline Vector2 CubicAt(const Vector2 p[4], float t) {
  const float u = 1.0f - t;
  const float a = u * u * u;
  const float b = 3.0f * t * u * u;
  const float c = 3.0f * t * t * u;
  const float d = t * t * t;
  return {a * p[0].x + b * p[1].x + c * p[2].x + d * p[3].x,
          a * p[0].y + b * p[1].y + c * p[2].y + d * p[3].y};
}

// The loop as it is actually closed: every sample, then the curve that bridges
// the gap. This is the shape, and both the drawing and the hit test use it.
inline std::vector<Vector2> ClosedLasso(const std::vector<Vector2>& lasso) {
  std::vector<Vector2> loop = lasso;
  Vector2              curve[4];
  if (!LassoClosure(lasso, curve)) return loop;
  // The endpoints are already in the list, so only the inside of the curve is
  // added.
  for (int i = 1; i < kLassoCloseSegments; ++i) {
    loop.push_back(CubicAt(curve, static_cast<float>(i) / kLassoCloseSegments));
  }
  return loop;
}

// Ray casting. The loop is closed implicitly, last point back to first.
inline bool PointInLasso(const std::vector<Vector2>& lasso, Vector2 point) {
  if (lasso.size() < 3) return false;
  bool inside = false;
  for (std::size_t i = 0, j = lasso.size() - 1; i < lasso.size(); j = i++) {
    const Vector2 a = lasso[i];
    const Vector2 b = lasso[j];
    if ((a.y > point.y) == (b.y > point.y)) continue;
    const float crossing = (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x;
    if (point.x < crossing) inside = !inside;
  }
  return inside;
}

// What the lasso catches: a window whose middle it went round. The middle
// rather than any overlap, so that what is caught is exactly what was circled.
inline bool LassoCatches(const std::vector<Vector2>& lasso, Rectangle rect) {
  return PointInLasso(lasso, {rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f});
}

// --------------------------------------------------------------- the windows
//
// Every window on this canvas answers the lasso the same way, so it is written
// once rather than six times. All any of these needs is that a window has a
// `selected` flag, a `worldPos` and a `dragFrom`, and that whoever holds a row
// of them knows where the n-th one is on screen. What differs between the kinds
// -- what closing one *means* -- stays with the kind, which is the only part
// that was ever really different.

// A panel holds its windows either by value or behind a pointer. This is how
// one is reached either way.
template <class T>
T& Held(T& item) {
  return item;
}
template <class T>
const T& Held(const T& item) {
  return item;
}
template <class T>
T& Held(std::unique_ptr<T>& item) {
  return *item;
}
template <class T>
T& Held(const std::unique_ptr<T>& item) {
  return *item;
}

template <class Items, class RectOf>
void SelectCaught(Items& items, const std::vector<Vector2>& lasso, RectOf rectOf) {
  for (std::size_t i = 0; i < items.size(); ++i) {
    Held(items[i]).selected = LassoCatches(lasso, rectOf(i));
  }
}

template <class Items>
void ClearSelected(Items& items) {
  for (auto& item : items) Held(item).selected = false;
}

template <class Items>
bool AnySelected(const Items& items) {
  for (const auto& item : items) {
    if (Held(item).selected) return true;
  }
  return false;
}

template <class Items, class RectOf>
bool SelectedRim(const Items& items, Vector2 point, RectOf rectOf) {
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (Held(items[i]).selected && PointOnRim(rectOf(i), point)) return true;
  }
  return false;
}

// Where everything is now, so a drag can place it relative to there rather than
// accumulating a delta frame by frame.
template <class Items>
void RememberPositions(Items& items) {
  for (auto& item : items) Held(item).dragFrom = Held(item).worldPos;
}

template <class Items>
void MoveSelected(Items& items, Vector2 worldDelta) {
  for (auto& item : items) {
    auto& window = Held(item);
    if (!window.selected) continue;
    // A docked window has no position of its own: it follows its box, and moves
    // when the box does. A kind with nothing to dock to skips the question.
    if constexpr (requires { window.docked; }) {
      if (window.docked) continue;
    }
    window.worldPos = {window.dragFrom.x + worldDelta.x, window.dragFrom.y + worldDelta.y};
  }
}

template <class Items, class RectOf>
void DrawSelectedRims(const Items& items, float zoom, RectOf rectOf) {
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (Held(items[i]).selected) DrawRim(rectOf(i), zoom, true);
  }
}

template <class Items>
void EraseSelected(Items& items) {
  items.erase(std::remove_if(items.begin(), items.end(),
                             [](const auto& item) { return Held(item).selected; }),
              items.end());
}

inline void DrawLasso(const std::vector<Vector2>& lasso) {
  if (lasso.size() < 2) return;
  for (std::size_t i = 1; i < lasso.size(); ++i) {
    DrawLineEx(lasso[i - 1], lasso[i], 2.0f, kLassoLine);
  }

  // The closure, drawn faintly: it is where the loop *will* close, not
  // somewhere the pointer has been.
  Vector2 curve[4];
  if (LassoClosure(lasso, curve)) {
    DrawSplineSegmentBezierCubic(curve[0], curve[1], curve[2], curve[3], 2.0f, kLassoClose);
  } else {
    DrawLineEx(lasso.back(), lasso.front(), 2.0f, kLassoClose);
  }
}

}  // namespace sb
