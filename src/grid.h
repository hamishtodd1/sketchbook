#pragma once

#include "viewport.h"

namespace sb {

// Draws the dotted square grid. The spacing halves and doubles in powers of two
// so the dots stay a comfortable distance apart at any zoom, and the
// intermediate dots fade rather than popping when the level changes.
void DrawDotGrid(const Viewport& viewport);

// World units between the finest dots drawn at this zoom. Windows snap their
// top-left corner to this lattice, so what you snap to is what you can see:
// zoom in and the placement gets finer along with the grid.
float GridStep(float zoom);

// The lattice point nearest a world position. Every window on the canvas is
// placed through this -- boxes, number lines and plots alike.
Vector2 SnapToGrid(Vector2 world, float zoom);

}  // namespace sb
