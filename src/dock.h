// Where a window a box makes goes, before anybody has moved it.
//
// **Everything a box *makes* flanks the card**: the first to the right, the
// second to the left, the third under the first, the fourth under the second,
// and so on down the two columns. Alternating rather than filling one column and
// then the other, so a box with two windows has one on each side and reads as
// balanced -- and so that nothing ever lands on top of anything, which it used
// to, because every kind independently asked for the same spot to the right.
//
// The ones a box is *read against* -- its number lines and its plots -- are not
// in this arrangement. They stay under the box, as wide as it is, because a
// number line is a scale the box's values are read on rather than a picture the
// box produced.
//
// Which kind takes which slot is settled by the order the panels are chained in
// (`App::Init`), and each kind only has to know how many slots the kinds before
// it have already taken. No one of them owns the arrangement.
#pragma once

#include <vector>

#include "raylib.h"

namespace sb {

class CodeBlock;

// One height for all of them, so the two columns line up in rows rather than
// stepping raggedly down the sides of the box. Most are square at that height;
// a space window is 16:9, the shape a view into a space is looked at in.
inline constexpr float kFlankSide = 320.0f;   // world units
inline constexpr float kFlankGap = 16.0f;
inline constexpr float kSpaceAspect = 16.0f / 9.0f;

// A kind of window that flanks a box.
class Flanking {
 public:
  virtual ~Flanking() = default;
  // How many of this kind are docked beside `block` right now.
  virtual int FlankCount(const CodeBlock* block) const = 0;
};

// How many slots the kinds before this one have already taken beside a box.
inline int FlankBase(const std::vector<const Flanking*>& before, const CodeBlock* block) {
  int taken = 0;
  for (const Flanking* panel : before) taken += panel->FlankCount(block);
  return taken;
}

// The n-th slot beside a card, in screen space. `zoom` scales the gap and the
// side the way it scales everything else on the canvas; `aspect` is how many
// times wider than tall this kind of window is.
inline Rectangle FlankRect(Rectangle card, int slot, float zoom, float aspect = 1.0f) {
  const float tall = kFlankSide * zoom;
  const float wide = tall * aspect;
  const float gap = kFlankGap * zoom;
  const int   column = slot & 1;            // 0 to the right, 1 to the left
  const int   row = slot >> 1;
  const float x = (column == 0) ? card.x + card.width + gap : card.x - gap - wide;
  return {x, card.y + static_cast<float>(row) * (tall + gap), wide, tall};
}

}  // namespace sb
