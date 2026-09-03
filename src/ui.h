#pragma once

#include "fontstore.h"
#include "raylib.h"

namespace sb {

// The shape a deck is made for unless it says otherwise, and the shape the app
// window itself opens at -- the same number, so a slide seen at its own scale
// fills a fresh window exactly.
inline constexpr float kDefaultSlideAspect = 3.0f / 2.0f;

// Canvas settings the menu can change.
struct Options {
  bool  showGridDots = true;
  // The shape a slide is made for. Everything about a slide follows from it:
  // how wide it is, and what the camera has to be at for one to fill the
  // window.
  float slideAspect = kDefaultSlideAspect;
};

// The shapes the menu offers. A deck is made for one of them, not for an
// arbitrary number typed in.
inline constexpr int   kAspectCount = 4;
inline constexpr float kAspects[kAspectCount] = {kDefaultSlideAspect, 16.0f / 9.0f,
                                                 4.0f / 3.0f, 1.0f};
inline constexpr const char* kAspectNames[kAspectCount] = {"3:2", "16:9", "4:3", "1:1"};

// What the menu is asking the app to do this frame.
enum class MenuAction { None, Save, Load, Insert };

// The Escape menu. Drawn in screen space at a fixed pixel size, so it does not
// take part in the canvas zoom.
class OptionsMenu {
 public:
  bool Visible() const { return visible_; }
  void Toggle() { visible_ = !visible_; }
  void Close() { visible_ = false; }

  MenuAction Update(Options* options);
  void       Draw(FontStore& fonts, const Options& options) const;

 private:
  bool visible_ = false;
};

}  // namespace sb
