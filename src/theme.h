// Central palette and canvas constants.
#pragma once

#include <algorithm>
#include <cmath>

#include "raylib.h"

namespace sb {

// ------------------------------------------------------------------- rims
// Every window on the canvas is moved by the band around its edge. It appears
// on hover, so nothing is drawn until you go looking for it.
//
// A rim is chrome, not content: it keeps the same thickness in screen pixels
// however far the canvas is zoomed, so it stays exactly as easy to hit at any
// scale. Everything else on the canvas is sized in world units and does scale.
inline constexpr Color kRim       = { 150, 172, 205, 235 };
inline constexpr Color kRimActive = { 108, 158, 230, 255 };
// raylib draws a rounded outline *outward* from the rectangle, so this is also
// how far the visible rim extends past the edge.
inline constexpr float kRimThickness = 5.1f;   // screen pixels
inline constexpr float kRimGrabInside = 10.0f;  // grab depth inside the edge

inline bool PointOnRim(Rectangle rect, Vector2 point) {
  const Rectangle grown = {rect.x - kRimThickness, rect.y - kRimThickness,
                           rect.width + 2.0f * kRimThickness,
                           rect.height + 2.0f * kRimThickness};
  if (!CheckCollisionPointRec(point, grown)) return false;

  const Rectangle core = {rect.x + kRimGrabInside, rect.y + kRimGrabInside,
                          rect.width - 2.0f * kRimGrabInside,
                          rect.height - 2.0f * kRimGrabInside};
  // A window too small to have an interior is all rim.
  if (core.width <= 0.0f || core.height <= 0.0f) return true;
  return !CheckCollisionPointRec(point, core);
}

inline void DrawRim(Rectangle rect, float zoom, bool active) {
  // The corner radius follows the card's, which does scale with the canvas.
  const float radius = 8.0f * zoom;
  const float minSide = std::max(1.0f, std::min(rect.width, rect.height));
  // Never let this reach zero: raylib bails out of a rounded outline whose
  // corner radius rounds away, and the rim would silently vanish.
  const float roundness = std::clamp(radius / (minSide * 0.5f), 0.02f, 1.0f);
  DrawRectangleRoundedLinesEx(rect, roundness, 10, kRimThickness,
                              active ? kRimActive : kRim);
}

// ----------------------------------------------------------- the drop target
// Carrying a window by its rim brings up a cross at the foot of the app window.
// Drag onto it and the window is absorbed -- it stops being drawn, and letting
// go there is what throws it away. Pull off again and nothing has happened.
//
// This is the only way to close a window, and it is the same gesture for every
// kind of window, which is why it lives out here with the rim rather than as a
// button on any one of them.
inline constexpr Color kDropBg      = {  56,  57,  50, 220 };
inline constexpr Color kDropHot     = { 236,  98,  92, 255 };
inline constexpr Color kDropMark    = { 226, 226, 220, 255 };
inline constexpr float kDropSide    = 62.0f;   // screen pixels
inline constexpr float kDropMargin  = 26.0f;

inline Rectangle DropTargetRect() {
  return {static_cast<float>(GetScreenWidth()) * 0.5f - kDropSide * 0.5f,
          static_cast<float>(GetScreenHeight()) - kDropSide - kDropMargin, kDropSide,
          kDropSide};
}

inline bool OverDropTarget(Vector2 point) {
  return CheckCollisionPointRec(point, DropTargetRect());
}

inline void DrawDropTarget(bool absorbing) {
  Rectangle rect = DropTargetRect();
  if (absorbing) {
    // It swells as it takes the window in, so the gesture reads before you let
    // go rather than after.
    const float grow = kDropSide * 0.14f;
    rect = {rect.x - grow, rect.y - grow, rect.width + 2.0f * grow, rect.height + 2.0f * grow};
  }
  DrawRectangleRounded(rect, 0.28f, 10, absorbing ? kDropHot : kDropBg);

  const float inset = rect.width * 0.32f;
  const float thickness = std::max(2.0f, rect.width * 0.055f);
  DrawLineEx({rect.x + inset, rect.y + inset},
             {rect.x + rect.width - inset, rect.y + rect.height - inset}, thickness, kDropMark);
  DrawLineEx({rect.x + rect.width - inset, rect.y + inset},
             {rect.x + inset, rect.y + rect.height - inset}, thickness, kDropMark);
}

// ------------------------------------------------------------- the pointer
// The canvas is dragged with the right button, and while it is being dragged
// the pointer becomes a **closed hand**: the gesture is holding the paper and
// moving it, and the cursor should say so rather than staying an arrow.
//
// It is drawn rather than chosen, because there is no closed-hand cursor in the
// set the window system offers -- GLFW's "hand" is the pointing one. Drawn in
// two passes, dark then light, so it reads against a pale canvas and a dark
// card alike.
inline constexpr Color kCursorFill = { 250, 250, 250, 255 };
inline constexpr Color kCursorEdge = {  26,  26,  30, 235 };

inline void DrawGrabCursor(Vector2 at) {
  // A palm, four knuckles folded over the top of it, and a thumb across the
  // side. The knuckles overlap slightly, which is what gives the scalloped top
  // a fist has; the creases between them are what stop the whole thing reading
  // as a lump at cursor size.
  const auto hand = [&](float grow, Color tint) {
    const Rectangle palm = {at.x - 11.0f - grow, at.y - 2.0f - grow, 22.0f + 2.0f * grow,
                            17.0f + 2.0f * grow};
    DrawRectangleRounded(palm, 0.45f, 10, tint);
    for (int i = 0; i < 4; ++i) {
      DrawCircleV({at.x - 7.5f + static_cast<float>(i) * 5.0f, at.y - 3.0f}, 4.3f + grow, tint);
    }
    DrawCircleV({at.x - 10.5f, at.y + 3.5f}, 5.4f + grow, tint);
  };
  hand(2.0f, kCursorEdge);
  hand(0.0f, kCursorFill);

  // The gaps between the fingers, and the line the thumb lies along.
  for (int i = 0; i < 3; ++i) {
    const float x = at.x - 5.0f + static_cast<float>(i) * 5.0f;
    DrawLineEx({x, at.y - 6.6f}, {x, at.y + 1.0f}, 1.5f, kCursorEdge);
  }
  DrawLineEx({at.x - 6.0f, at.y + 5.0f}, {at.x + 4.0f, at.y + 3.0f}, 1.5f, kCursorEdge);
}

// ------------------------------------------------------------------ canvas
inline constexpr Color kCanvasBg   = { 232, 232, 232, 255 };  // pure light gray
inline constexpr Color kGridDot    = { 148, 148, 148, 255 };

// World units between the finest grid dots at zoom 1.
inline constexpr float kGridBaseStep = 40.0f;
// The grid subdivides/coarsens to keep the on-screen dot spacing in this band.
// The band spans exactly one doubling, and zoom 1 sits well inside it so the
// intermediate dots read as a full grid rather than a faint half-grid.
inline constexpr float kGridMinPx    = 24.0f;
inline constexpr float kGridMaxPx    = 48.0f;

// ------------------------------------------------------------------- cards
// Monokai, background included.
inline constexpr Color kCodeBg     = {  39,  40,  34, 255 };
inline constexpr Color kCodeFg     = { 248, 248, 242, 255 };
inline constexpr Color kMathBg     = { 255, 255, 255, 255 };  // pure white
inline constexpr Color kMathFg     = {   0,   0,   0, 255 };
inline constexpr Color kCardShadow = {   0,   0,   0,  38 };
inline constexpr Color kCaret      = { 248, 248, 242, 255 };

// Monokai token colours.
inline constexpr Color kTokComment  = { 117, 113,  94, 255 };
inline constexpr Color kTokKeyword  = { 249,  38, 114, 255 };
inline constexpr Color kTokType     = { 102, 217, 239, 255 };
inline constexpr Color kTokNumber   = { 174, 129, 255, 255 };
inline constexpr Color kTokString   = { 230, 219, 116, 255 };
inline constexpr Color kTokFunction = { 166, 226,  46, 255 };
inline constexpr Color kTokOperator = { 249,  38, 114, 255 };

// Box chrome, shown only while a box has the caret.
inline constexpr Color kChromeBg     = {  56,  57,  50, 255 };
inline constexpr Color kChromeHover  = {  84,  85,  74, 255 };
inline constexpr Color kChromeText   = { 226, 226, 220, 255 };

// The compiler error balloon: black on white.
inline constexpr Color kErrorBg      = { 255, 255, 255, 255 };
inline constexpr Color kErrorFg      = {   0,   0,   0, 255 };

// The injected initialiser shown on hover. Distinctly not-yours-to-edit.
inline constexpr Color kInjected = { 122, 162, 247, 255 };

// ---------------------------------------------------------------- axes
inline constexpr Color kAxisBg        = {  28,  28,  32, 255 };
inline constexpr Color kAxisBorder    = {  62,  62,  70, 255 };
inline constexpr Color kAxisLine      = { 132, 132, 142, 255 };
inline constexpr Color kAxisTick      = {  96,  96, 106, 255 };
inline constexpr Color kAxisOrigin    = { 176, 176, 188, 255 };
inline constexpr Color kAxisTickLabel = { 130, 130, 140, 255 };
inline constexpr Color kAxisLabel     = { 236, 236, 240, 255 };
inline constexpr Color kAxisValue     = { 186, 186, 196, 255 };
inline constexpr Color kAxisPending   = { 110, 110, 120, 255 };

// -------------------------------------------------------------------- media
// A picture is furniture: it wears no card and no background, so what you see
// is the picture. Everything drawn over one is chrome, and appears on hover.
inline constexpr Color kMediaBroken   = {  62,  62,  68, 255 };
inline constexpr Color kTransportBg   = {  16,  16,  18, 190 };
inline constexpr Color kTransportFg   = { 236, 236, 240, 255 };
inline constexpr Color kBarTrack      = { 108, 108, 116, 210 };
inline constexpr Color kBarClip       = { 238, 238, 244, 240 };
inline constexpr Color kBarPlayed     = { 108, 158, 230, 255 };
inline constexpr Color kTrimMark      = { 246, 208,  92, 255 };
// The corner brackets that crop. Distinct from the rim, because they do a
// different thing: the rim moves the window, a corner moves the frame.
inline constexpr Color kCropHandle    = { 250, 250, 250, 200 };
inline constexpr Color kCropHandleHot = { 108, 158, 230, 255 };

// ------------------------------------------------------------------- slides
// A slide is a region of the canvas rather than a card on it: nothing is
// painted behind its text, so the text is ink on the canvas itself.
inline constexpr Color kSlideInk = { 28, 28, 30, 255 };

// ----------------------------------------------------------------- variables
// Every variable has a colour of its own, and everything standing for that
// variable wears it: the handle on its number line, its curve on a plot, and
// the box drawn round its name in the equation. Colour is how you see that two
// windows are about the same thing.
//
// They come from **viridis**, so they are perceptually even -- two colours the
// same distance apart on the map look equally different, which is exactly what
// is wanted of a set of labels.
inline Color Viridis(float t) {
  static constexpr int kStops[][3] = {
      { 68,   1,  84}, { 72,  40, 120}, { 62,  74, 137}, { 49, 104, 142},
      { 38, 130, 142}, { 31, 158, 137}, { 53, 183, 121}, {109, 205,  89},
      {180, 222,  44}, {223, 227,  24}, {253, 231,  37},
  };
  constexpr int kCount = static_cast<int>(sizeof(kStops) / sizeof(kStops[0]));

  const float position = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(kCount - 1);
  const int   low = std::min(static_cast<int>(position), kCount - 2);
  const float fraction = position - static_cast<float>(low);

  auto mix = [&](int channel) {
    const float value = static_cast<float>(kStops[low][channel]) +
                        static_cast<float>(kStops[low + 1][channel] - kStops[low][channel]) *
                            fraction;
    return static_cast<unsigned char>(value + 0.5f);
  };
  return {mix(0), mix(1), mix(2), 255};
}

// The colour of the n-th variable in its box.
inline Color VariableColor(int index) {
  // Walking the map by the golden ratio keeps consecutive variables as far
  // apart as is possible without knowing how many there will be, and adding one
  // never changes the colour of the others.
  constexpr float kGolden = 0.6180339887f;
  float t = static_cast<float>(index) * kGolden;
  t -= std::floor(t);
  // The dark quarter of viridis is skipped: those purples disappear against a
  // card that is already dark.
  return Viridis(0.28f + 0.72f * t);
}

// The box drawn round a variable's name while its window is being used.
inline constexpr float kMentionFillAlpha = 0.24f;

// ------------------------------------------------------- compilation result
inline constexpr Color kResultTrue    = { 214,  62,  58, 255 };  // testFunc() == true
inline constexpr Color kResultFalse   = {  46, 104, 214, 255 };  // testFunc() == false
inline constexpr Color kResultUnknown = { 178, 178, 182, 255 };  // building, or broken
inline constexpr Color kErrorCaret    = { 236,  68,  62, 255 };

// -------------------------------------------------------------------- menu
inline constexpr Color kScrim       = {  18,  18,  20, 130 };
inline constexpr Color kPanelBg     = { 250, 250, 250, 255 };
inline constexpr Color kPanelBorder = { 205, 205, 205, 255 };
inline constexpr Color kPanelText   = {  28,  28,  30, 255 };
inline constexpr Color kPanelMuted  = { 122, 122, 128, 255 };
inline constexpr Color kAccent      = {  38, 110, 220, 255 };
inline constexpr Color kAccentHover = {  60, 132, 238, 255 };
inline constexpr Color kBoxIdle     = { 255, 255, 255, 255 };
inline constexpr Color kBoxBorder   = { 176, 176, 182, 255 };

// ------------------------------------------------------------------- zoom
inline constexpr float kMinZoom = 0.05f;
inline constexpr float kMaxZoom = 40.0f;

}  // namespace sb
