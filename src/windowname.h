// The name of the display window under the pointer, written above it.
//
// A viz box names the window it is drawn in -- `Viz(Bi, "Space")` -- so the
// name has to be findable from the window too, or writing one is a guess. It
// is the name the viz box uses, exactly, and it appears where a rim does: while
// the window is pointed at or carried.
//
// A window offers its name as it draws, and the app writes the last one offered
// after everything else, so a neighbour drawn later cannot cover it. Only one
// window is under the pointer at a time, so one slot is enough.
#pragma once

#include <string>

#include "fontstore.h"
#include "raylib.h"
#include "text.h"
#include "theme.h"

namespace sb::windowname {

struct Pending {
  bool        any = false;
  Rectangle   rect{};
  std::string name;
};

inline Pending& Slot() {
  static Pending pending;
  return pending;
}

// A dw being pointed at, or carried, says what it is called.
inline void Offer(Rectangle rect, const char* name) { Slot() = {true, rect, name}; }

// Written just above the window's top-left corner, clear of its rim, at a fixed
// size on screen like every other piece of chrome. Then forgotten: it is
// offered again next frame if the window is still being pointed at.
inline void Draw(FontStore& fonts) {
  Pending& pending = Slot();
  if (!pending.any) return;
  constexpr float kEm = 15.0f;
  constexpr float kPadX = 7.0f;
  constexpr float kPadY = 3.0f;
  const float wide = text::Measure(fonts, Face::MainRegular, pending.name, kEm) + kPadX * 2.0f;
  const float tall = text::LineHeight(fonts, Face::MainRegular, kEm) + kPadY * 2.0f;
  const Rectangle tag = {pending.rect.x, pending.rect.y - kRimThickness - 3.0f - tall, wide, tall};
  DrawRectangleRounded(tag, 0.35f, 8, kRimActive);
  text::Draw(fonts, Face::MainRegular, pending.name,
             {tag.x + kPadX, tag.y + kPadY + text::Ascent(fonts, Face::MainRegular, kEm)}, kEm,
             RAYWHITE);
  pending = {};
}

}  // namespace sb::windowname
