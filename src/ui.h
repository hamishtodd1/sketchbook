#pragma once

#include <string>
#include <vector>

#include "fontstore.h"
#include "raylib.h"

namespace sb {

// The shape a deck is made for unless it says otherwise, and the shape the app
// window itself opens at -- the same number, so a slide seen at its own scale
// fills a fresh window exactly.
inline constexpr float kDefaultSlideAspect = 3.0f / 2.0f;

// Canvas settings the menu can change.
struct Options {
  // The shape a slide is made for. Everything about a slide follows from it:
  // how wide it is, and what the camera has to be at for one to fill the
  // window.
  float slideAspect = kDefaultSlideAspect;
  // Whether the app is filling the screen. About the window rather than the
  // canvas, so it is not saved; it is here so the menu can say which way its
  // button goes.
  bool fullscreen = false;
};

// The shapes the menu offers. A deck is made for one of them, not for an
// arbitrary number typed in.
inline constexpr int   kAspectCount = 4;
inline constexpr float kAspects[kAspectCount] = {kDefaultSlideAspect, 16.0f / 9.0f,
                                                 4.0f / 3.0f, 1.0f};
inline constexpr const char* kAspectNames[kAspectCount] = {"3:2", "16:9", "4:3", "1:1"};

// What the menu is asking the app to do this frame.
enum class MenuAction { None, NewCanvas, Save, Load, Insert, NewSlide, OpenRecent, Fullscreen };

// The Escape menu. Drawn in screen space at a fixed pixel size, so it does not
// take part in the canvas zoom.
class OptionsMenu {
 public:
  bool Visible() const { return visible_; }
  // Opening reads the recent list off disk. It is a handful of lines and it is
  // only read when the menu comes up, so it can be read afresh each time and
  // never go stale against another window of the app.
  void Toggle();
  void Close() { visible_ = false; }

  MenuAction Update(Options* options);
  void       Draw(FontStore& fonts, const Options& options) const;

  // Which canvas an OpenRecent is asking for.
  const std::string& Chosen() const { return chosen_; }

 private:
  bool visible_ = false;
  // Whether the slide shapes are showing. A shape is chosen once for a deck and
  // then left alone, so the four of them are folded away by default; this is a
  // fact about the menu rather than about the canvas, so it is not saved.
  bool                     aspectOpen_ = false;
  std::vector<std::string> recent_;
  std::string              chosen_;
};

}  // namespace sb
