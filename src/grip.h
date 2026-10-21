// The resize grip: three short diagonals in a window's bottom-right corner.
//
// It began as the conversation's, and is now every window's: **dragging it is
// how a window is resized**. A conversation's corner sets its width and height
// apart, its words reflowing; everything else **keeps its shape** -- the grip
// scales it uniformly, so nothing is ever stretched -- with its top-left corner
// held still, so the window grows away from the hand rather than out from under
// it. For a box of text or code what scales is the type; for a picture, the
// picture.
#pragma once

#include <algorithm>

#include "fontstore.h"
#include "raylib.h"
#include "theme.h"
#include "viewport.h"

namespace sb {

inline constexpr float kGripSide = 14.0f;   // screen pixels, like all chrome

// Where the grip sits in a window drawn at `rect`, on screen: just inside the
// corner, clear of the rim's grab band, so carrying and resizing never want the
// same pixel -- and above `footer` pixels of its foot (a video's transport).
inline Rectangle GripRect(Rectangle rect, float footer = 0.0f) {
  const float smaller = std::min(rect.width, rect.height - footer);
  const float side = std::min(kGripSide, smaller * 0.3f);
  const float inset = std::min(kRimGrabInside, smaller * 0.2f);
  return {rect.x + rect.width - inset - side, rect.y + rect.height - footer - inset - side, side,
          side};
}

inline void DrawGrip(Rectangle grip, Color ink, float thickness = 1.5f) {
  for (int k = 1; k <= 3; ++k) {
    const float o = grip.width * static_cast<float>(k) / 3.0f;
    DrawLineEx({grip.x + grip.width, grip.y + grip.height - o},
               {grip.x + grip.width - o, grip.y + grip.height}, thickness, ink);
  }
}

// On a window whose inside could be any colour -- a picture, a sky -- dark
// under light, so it reads on both.
inline void DrawGripOnAnything(Rectangle grip) {
  DrawGrip(grip, Fade(BLACK, 0.45f), 3.0f);
  DrawGrip(grip, Fade(WHITE, 0.9f), 1.5f);
}

// **A display window, as the grip sees it**: how many of a kind there are,
// where each one is on screen, how to make one come loose from its box -- which
// resizing does, docking being slots of one size -- and how big it is, as a
// multiple of the size every window of its kind starts at.
class Resizable {
 public:
  virtual ~Resizable() = default;
  virtual int       GripCount() const = 0;
  virtual Rectangle GripWindow(int index, const Viewport& viewport, FontStore& fonts) const = 0;
  virtual void      Loosen(int index, const Viewport& viewport, FontStore& fonts) = 0;
  virtual float     WindowSize(int index) const = 0;
  virtual void      SetWindowSize(int index, float size) = 0;
};

// The shared body of `Loosen`, for a window with `docked` and `worldPos`: it
// stays exactly where it is on screen, and is its box's no longer.
template <typename Item>
void LoosenWindow(Item& item, Rectangle screen, const Viewport& viewport) {
  if (!item.docked) return;
  item.worldPos = viewport.ToWorld({screen.x, screen.y});
  item.docked = false;
}

inline constexpr float kMinWindowSize = 0.25f;
inline constexpr float kMaxWindowSize = 12.0f;

// How much a window that was `start` (world) should be scaled now that the
// pointer has gone from `from` to `now` (world): the pointer's travel along the
// window's own diagonal, so the dragged corner follows the hand while the shape
// stays the shape.
inline float GripScale(Rectangle start, Vector2 from, Vector2 now) {
  const float lengthSq = start.width * start.width + start.height * start.height;
  if (!(lengthSq > 0.0f)) return 1.0f;
  const float along = (now.x - from.x) * start.width + (now.y - from.y) * start.height;
  return std::max(0.05f, 1.0f + along / lengthSq);
}

}  // namespace sb
