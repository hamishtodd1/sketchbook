// What a carried window is following: the mouse, or one finger.
//
// Every panel carries a window the same way -- it follows the pointer until the
// button comes up -- and for a mouse that is exactly right. It is not right for
// a window pulled out of another by two fingers, one holding the window and one
// dragging the thing out of it: the system's emulated mouse follows the *first*
// finger, the one standing still, and the window would stay where it was put
// down. So the app can hand a carry to a finger, and the panels ask here rather
// than asking the mouse directly.
//
// Only the code that carries and drops a window reads this. Nothing else about
// the pointer changes while a finger has the carry.
#pragma once

#include "raylib.h"

namespace sb::carry {

struct Hand {
  bool    finger = false;   // a finger has the carry; otherwise it is the mouse's
  Vector2 at{};
  bool    down = false;
  bool    wasDown = false;
};

inline Hand& State() {
  static Hand hand;
  return hand;
}

// Called once a frame by the app while a finger has the carry: where that finger
// is, and whether it is still down. Lifting is reported as one frame with `down`
// false, so the panel carrying the window sees it let go.
inline void FollowFinger(Vector2 at, bool down) {
  Hand& hand = State();
  hand.wasDown = hand.finger ? hand.down : true;
  hand.finger = true;
  hand.at = at;
  hand.down = down;
}

// Back to the mouse.
inline void FollowMouse() { State() = Hand{}; }

inline bool ByFinger() { return State().finger; }

inline Vector2 Position() { return State().finger ? State().at : GetMousePosition(); }

inline bool Down(int button) {
  return State().finger ? State().down : IsMouseButtonDown(button);
}

inline bool Released(int button) {
  return State().finger ? (!State().down && State().wasDown) : IsMouseButtonReleased(button);
}

}  // namespace sb::carry
