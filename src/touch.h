// Multi-touch input.
//
// raylib's GetTouchPointCount()/GetTouchPosition() only carry real data on
// Android and web; on the Windows desktop backend GLFW synthesises a single
// touch point from the mouse, so a two-finger pinch is invisible to it. This
// module subclasses the window procedure and reads WM_POINTER messages
// directly, which is how a Win8+ app gets genuine multi-touch.
//
// Legacy mouse messages synthesised from touch are deliberately left alone, so
// a finger tap still reaches raylib's mouse API and can drive the menu; the
// viewport ignores mouse panning while any finger is down.
#pragma once

#include "raylib.h"

namespace sb::touch {

struct Point {
  unsigned int id = 0;
  Vector2      position{};  // client-area pixels, matching raylib mouse coords
};

inline constexpr int kMaxPoints = 10;

// Call once after InitWindow. A no-op on platforms without a backend.
void Install();
void Shutdown();

// True when a real multi-touch source is wired up.
bool Available();

int          Count();
const Point* Points();

}  // namespace sb::touch
