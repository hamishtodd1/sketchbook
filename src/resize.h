// Snapping a window's size to its neighbours'.
//
// A window is resized by its grip (grip.h), uniformly -- one number applied to
// both dimensions, so nothing is stretched out of shape. **It snaps to its
// neighbours**: two pictures side by side should be able to be exactly the same
// height, and a caption exactly as wide as what it is under. Nothing else on
// this canvas can express "the same size as that one", so the resize gesture
// has to.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "raylib.h"

namespace sb {

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

// The scale to actually apply, given the one the grip asked for. Every nearby
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

}  // namespace sb
