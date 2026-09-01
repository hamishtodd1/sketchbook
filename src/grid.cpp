#include "grid.h"

#include <algorithm>
#include <cmath>

#include "theme.h"

namespace sb {

float GridStep(float zoom) {
  if (!(zoom > 0.0f)) return kGridBaseStep;

  // Pick a power-of-two world spacing whose on-screen size lands in the band.
  float step = kGridBaseStep;
  int guard = 0;
  while (step * zoom < kGridMinPx && guard++ < 64) step *= 2.0f;
  guard = 0;
  while (step * zoom > kGridMaxPx && guard++ < 64) step *= 0.5f;
  return step;
}

Vector2 SnapToGrid(Vector2 world, float zoom) {
  const float step = GridStep(zoom);
  return {std::round(world.x / step) * step, std::round(world.y / step) * step};
}

void DrawDotGrid(const Viewport& viewport) {
  const float zoom = viewport.Zoom();
  if (!(zoom > 0.0f)) return;

  const float step = GridStep(zoom);
  const float spacing = step * zoom;
  // Intermediate dots fade in as they separate, so doubling the level is
  // invisible instead of a pop.
  const float t = std::clamp((spacing - kGridMinPx) / (kGridMaxPx - kGridMinPx), 0.0f, 1.0f);

  const float w = static_cast<float>(GetScreenWidth());
  const float h = static_cast<float>(GetScreenHeight());
  const Vector2 topLeft = viewport.ToWorld({0.0f, 0.0f});
  const Vector2 bottomRight = viewport.ToWorld({w, h});

  const long long i0 = static_cast<long long>(std::floor(topLeft.x / step)) - 1;
  const long long i1 = static_cast<long long>(std::ceil(bottomRight.x / step)) + 1;
  const long long j0 = static_cast<long long>(std::floor(topLeft.y / step)) - 1;
  const long long j1 = static_cast<long long>(std::ceil(bottomRight.y / step)) + 1;

  Color coarse = kGridDot;
  Color fine = kGridDot;
  fine.a = static_cast<unsigned char>(kGridDot.a * t);

  const float coarseSize = 3.0f;
  const float fineSize = 2.0f + t;

  for (long long j = j0; j <= j1; ++j) {
    for (long long i = i0; i <= i1; ++i) {
      // Dots on the even sublattice survive when the level doubles, so they are
      // the ones drawn at full strength.
      const bool onCoarse = ((i % 2 == 0) && (j % 2 == 0));
      if (!onCoarse && fine.a == 0) continue;

      const Vector2 p =
          viewport.ToScreen({static_cast<float>(i) * step, static_cast<float>(j) * step});
      if (p.x < -4.0f || p.x > w + 4.0f || p.y < -4.0f || p.y > h + 4.0f) continue;

      const float size = onCoarse ? coarseSize : fineSize;
      const float half = size * 0.5f;
      // Snap to whole pixels; a 2px dot straddling a pixel boundary just looks
      // like grey mush.
      DrawRectangleV({std::round(p.x) - half, std::round(p.y) - half}, {size, size},
                     onCoarse ? coarse : fine);
    }
  }
}

}  // namespace sb
