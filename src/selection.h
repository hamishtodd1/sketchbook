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
#pragma once

#include <vector>

#include "raylib.h"
#include "theme.h"

namespace sb {

inline constexpr Color kLassoLine  = {  38, 110, 220, 235 };
inline constexpr Color kLassoClose = {  38, 110, 220,  90 };
// Points nearer than this to the last one are not worth remembering.
inline constexpr float kLassoStep = 4.0f;

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

inline void DrawLasso(const std::vector<Vector2>& lasso) {
  if (lasso.size() < 2) return;
  for (std::size_t i = 1; i < lasso.size(); ++i) {
    DrawLineEx(lasso[i - 1], lasso[i], 2.0f, kLassoLine);
  }
  // The closing edge, drawn faintly: it is where the loop *will* close, not
  // somewhere the pointer has been.
  DrawLineEx(lasso.back(), lasso.front(), 2.0f, kLassoClose);
}

}  // namespace sb
