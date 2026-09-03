// The pan/zoom camera for the canvas.
//
// Everything on the canvas is drawn in screen space via ToScreen() rather than
// through BeginMode2D. That is deliberate: text has to be rasterised at its
// final on-screen pixel size to stay sharp, which means the zoom has to be
// folded into the font size rather than into a transform matrix.
#pragma once

#include <cstdint>

#include "raylib.h"
#include "touch.h"

namespace sb {

class Viewport {
 public:
  void Reset(Vector2 worldCentre, float zoom);

  // `interactive` is false while a modal (the options menu) has input.
  // `pointerFree` is false when a window under the pointer has claimed it: the
  // canvas then gives up the wheel and touch.
  // `panFree` is false while a window is using the right button for a scroll of
  // its own -- a number line's, so far. The canvas pans on the right button
  // wherever the pointer is, so the one window that wants it has to say so.
  void Update(bool interactive, bool pointerFree, bool panFree = true);

  // Glides to a view rather than jumping to it -- eased in and out, and with
  // the zoom moving geometrically, since doubling and halving are what a zoom
  // does evenly. Any gesture cancels it: the canvas is never taken away from
  // whoever is using it.
  void GlideTo(Vector2 worldCentre, float zoom, float seconds);
  bool Gliding() const { return gliding_; }

  Vector2 ToScreen(Vector2 world) const;
  Vector2 ToWorld(Vector2 screen) const;

  float   Zoom() const { return zoom_; }
  Vector2 Centre() const { return target_; }
  bool    Panning() const { return mouseDragging_ || touchCount_ > 0; }
  // Panning with the mouse in particular: a finger moving the canvas has no
  // cursor to turn into a hand.
  bool    PanningWithMouse() const { return mouseDragging_; }

 private:
  void PanByScreenDelta(Vector2 delta);
  void ZoomAbout(Vector2 screenAnchor, float factor);
  void UpdateTouch();
  void UpdateMouse(bool wheelToCanvas, bool panToCanvas);

  Vector2 target_{0.0f, 0.0f};  // world point sitting at the screen centre
  float   zoom_ = 1.0f;

  // Gesture bookkeeping.
  bool         mouseDragging_ = false;
  int          touchCount_ = 0;
  bool         haveTouchBaseline_ = false;
  unsigned int baselineIds_[touch::kMaxPoints]{};
  Vector2      baselineCentroid_{};
  float        baselineSpread_ = 0.0f;
  // Touch also emits legacy mouse events; this keeps the mouse path quiet for
  // a few frames after the last finger lifts so a pan does not double-apply.
  int          touchCooldown_ = 0;

  // A glide in flight.
  bool    gliding_ = false;
  Vector2 glideFromCentre_{};
  Vector2 glideToCentre_{};
  float   glideFromZoom_ = 1.0f;
  float   glideToZoom_ = 1.0f;
  float   glideElapsed_ = 0.0f;
  float   glideSeconds_ = 0.0f;
};

}  // namespace sb
