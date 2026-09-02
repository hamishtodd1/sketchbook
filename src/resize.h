// Resizing a window with the wheel.
//
// The gesture is the same for every kind of window: put the pointer on one and
// scroll, and it grows or shrinks **uniformly** -- one number, applied to both
// dimensions, so nothing is ever stretched out of shape. For a box of text or
// code that number is the size of the type; for a picture it is how big the
// picture is. Two fingers pinching do the same thing, since a trackpad pinch
// arrives as a wheel.
//
// Two rules make it feel like a handle rather than a slider:
//
// **The nearest corner stays where it is.** Whichever corner you are pointing
// at is the one that does not move, so the window grows away from your hand
// instead of out from under it.
//
// **It snaps to its neighbours.** Two pictures side by side should be able to
// be exactly the same height, and a caption should be able to be exactly as
// wide as what it is under. Nothing else on this canvas can express "the same
// size as that one", so the resize gesture has to.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "raylib.h"

namespace sb {

// One wheel notch.
inline constexpr float kResizeStep = 1.10f;

// How close a size has to be to a neighbour's before it is taken to mean it.
inline constexpr float kResizeSnapTolerance = 0.055f;
// How far away a neighbour can be and still be "near", as a multiple of the
// window's own size. Beyond that, matching sizes says nothing.
inline constexpr float kResizeSnapReach = 2.2f;

// True if `other` is close enough to `mine` to be worth matching. Both world
// rectangles.
inline bool NearEnoughToSnap(Rectangle mine, Rectangle other) {
  const float reachX = std::max(mine.width, other.width) * kResizeSnapReach;
  const float reachY = std::max(mine.height, other.height) * kResizeSnapReach;
  const Rectangle grown = {mine.x - reachX, mine.y - reachY, mine.width + 2.0f * reachX,
                           mine.height + 2.0f * reachY};
  return CheckCollisionRecs(grown, other);
}

// The scale to actually apply, given the one the wheel asked for. Every nearby
// window offers two candidates -- its height and its width -- and whichever is
// closest to what was asked wins, if it is close at all. Because the resize is
// uniform, matching either dimension fixes the scale, so the two kinds of
// neighbour are the same question asked twice.
inline float SnapResizeScale(Rectangle mine, float wanted,
                             const std::vector<Rectangle>& neighbours) {
  if (!(mine.width > 0.0f) || !(mine.height > 0.0f)) return wanted;

  float best = wanted;
  float bestError = kResizeSnapTolerance;
  const auto consider = [&](float candidate) {
    if (!(candidate > 0.0f) || !std::isfinite(candidate)) return;
    const float error = std::abs(candidate - wanted) / wanted;
    if (error < bestError) {
      bestError = error;
      best = candidate;
    }
  };

  for (const Rectangle& other : neighbours) {
    if (!(other.width > 0.0f) || !(other.height > 0.0f)) continue;
    if (!NearEnoughToSnap(mine, other)) continue;
    consider(other.height / mine.height);
    consider(other.width / mine.width);
  }
  return best;
}

// The corner of `rect` nearest the pointer -- the one that stays put.
inline Vector2 NearestCorner(Rectangle rect, Vector2 point) {
  const float x = (point.x < rect.x + rect.width * 0.5f) ? rect.x : rect.x + rect.width;
  const float y = (point.y < rect.y + rect.height * 0.5f) ? rect.y : rect.y + rect.height;
  return {x, y};
}

}  // namespace sb
