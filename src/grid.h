#pragma once

#include "viewport.h"

namespace sb {

// Draws the dotted square grid, at `strength` from 0 (nothing) to 1 (full).
//
// The grid is a placement aid rather than scenery, so it is only up while
// something is being placed -- a window carried, a window resized, the canvas
// zoomed -- and the caller decides that. Passing a strength rather than a flag
// is what lets it fade in and out instead of blinking.
//
// The spacing halves and doubles in powers of two so the dots stay a
// comfortable distance apart at any zoom, and the intermediate dots fade rather
// than popping when the level changes.
void DrawDotGrid(const Viewport& viewport, float strength);

// The finest spacing there is, in world units, before the doubling below.
//
// **One dot per line of type.** It is set once at startup from the height of a
// line at the default size, so the grid is not an arbitrary lattice laid over
// the canvas: it is ruled paper, and a box put down on it has its lines sitting
// in the gaps between rows of dots rather than straddling them. Measured rather
// than written down, since the font decides it.
void  SetGridBaseStep(float world);
float GridBaseStep();

// World units between the finest dots drawn at this zoom. Windows snap to this
// lattice, so what you snap to is what you can see: zoom in and the placement
// gets finer along with the grid.
float GridStep(float zoom);

// The lattice point nearest a world position. Every window on the canvas is
// placed through this -- boxes, number lines and plots alike.
Vector2 SnapToGrid(Vector2 world, float zoom);

}  // namespace sb
