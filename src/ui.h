#pragma once

#include "fontstore.h"
#include "raylib.h"

namespace sb {

// Canvas settings the menu can change.
struct Options {
  bool showGridDots = true;
};

// What the menu is asking the app to do this frame.
enum class MenuAction { None, Save, Load };

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
