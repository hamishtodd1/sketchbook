// The size of what is being drawn: the window -- or, while the editor renders a
// video (editor.h), the video's frame, which need not be the window's size.
//
// Everything that lays the canvas out on screen asks this rather than raylib,
// so a frame can be drawn at 1920 by 1080 into a target of that size whatever
// shape the window is.
#pragma once

#include "raylib.h"

namespace sb {

inline int& ScreenOverrideWidth() {
  static int width = 0;
  return width;
}
inline int& ScreenOverrideHeight() {
  static int height = 0;
  return height;
}
// 0 by 0 is the window again.
inline void SetScreenOverride(int width, int height) {
  ScreenOverrideWidth() = width;
  ScreenOverrideHeight() = height;
}
inline int ScreenWidth() {
  return ScreenOverrideWidth() > 0 ? ScreenOverrideWidth() : GetScreenWidth();
}
inline int ScreenHeight() {
  return ScreenOverrideHeight() > 0 ? ScreenOverrideHeight() : GetScreenHeight();
}

// **Drawing into a target while drawing into another.** A window that draws
// into its own render texture mid-frame -- the space window, a function being
// sampled -- ends with raylib's EndTextureMode, which goes back to the window.
// While the frame itself is being drawn into a target, that is the wrong place:
// these say where the frame is going, and go back there.
inline RenderTexture2D*& OuterTarget() {
  static RenderTexture2D* target = nullptr;
  return target;
}
inline void ResumeOuterTarget() {
  if (OuterTarget() != nullptr) BeginTextureMode(*OuterTarget());
}
inline void EndInnerTexture() {
  EndTextureMode();
  ResumeOuterTarget();
}

}  // namespace sb
