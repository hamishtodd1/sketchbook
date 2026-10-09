// Where the pointer is, as far as *hovering* is concerned.
//
// **Nothing is hovered while something is held** -- or while the Escape menu is
// up, which is the same situation: the pointer is busy elsewhere. A knob being
// dragged, a window being turned, carried or cropped, the canvas being panned, a
// lasso being drawn: the pointer is doing that, and whatever it passes over on the way
// is not being pointed at. Lighting it up anyway -- a rim appearing, a line
// flipping to code, a marker blinking, another box's windows jumping to the row
// under the cursor -- is the canvas answering a question nobody asked, in the
// middle of a gesture that wants the screen to hold still.
//
// So the gesture's own code reads the real pointer, as it always has, and every
// hover reads this one instead: the real pointer normally, and nowhere at all
// while a gesture is in flight. What the held thing says about itself -- the
// knob's own highlight, the carried window's rim, the drop target swelling --
// is not hover and keeps the real pointer.
//
// The app decides once a frame, after input, whether anything is held. That is
// the one place every gesture is already listed, so nothing has to be taught.
#pragma once

#include "raylib.h"

namespace sb::hover {

inline bool& HeldFlag() {
  static bool held = false;
  return held;
}

inline void SetHeld(bool held) { HeldFlag() = held; }
inline bool Held() { return HeldFlag(); }

// **A window drawn over everything covers what is under it.** A conversation
// sits on top of every other window, so while the pointer is on one, nothing
// beneath it is being pointed at -- a number line half under it does not light
// up, and a box under it does not flip to code. Set once a frame by the app.
inline bool& CoveredFlag() {
  static bool covered = false;
  return covered;
}
inline void SetCovered(bool covered) { CoveredFlag() = covered; }

// Nowhere is far enough off every edge that no rectangle can contain it.
inline Vector2 Pointer() {
  return (Held() || CoveredFlag()) ? Vector2{-1.0e9f, -1.0e9f} : GetMousePosition();
}
// What the covering window itself sees: the pointer, unless a gesture is held.
inline Vector2 TopPointer() { return Held() ? Vector2{-1.0e9f, -1.0e9f} : GetMousePosition(); }

}  // namespace sb::hover
