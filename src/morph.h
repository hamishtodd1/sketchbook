// A smooth transition between two pieces of typeset text, as filled outlines.
//
// Each side is a list of **pieces**: the things that move as a whole -- an
// identifier, a symbol, a fraction's rule -- each with a key saying what it is.
// A piece on one side and a piece with the same key on the other are the same
// thing, written two ways, so it travels from one to the other and its outline
// turns into the other's on the way; the k-th of a key matches the k-th, in
// reading order. A piece with no partner fades where it is. That is all a
// transition is: no colour (the caller fades its own), no knowledge of code or
// LaTeX (transition.h makes the pieces).
//
// Outlines are closed contours in pixels, y down, filled by the non-zero rule:
// what is a hole runs the other way round from what it is cut out of.
#pragma once

#include <string>
#include <vector>

namespace sb::morph {

struct Point {
  float x = 0.0f;
  float y = 0.0f;
};

using Contour = std::vector<Point>;

struct Piece {
  std::string          key;
  std::vector<Contour> contours;
};

// What to fill, at one moment.
struct Shape {
  std::vector<Contour> contours;
  float                alpha = 1.0f;
};

// The easing every transition moves by: still at both ends, quickest halfway.
float Ease(float t);

class Morph {
 public:
  Morph() = default;
  Morph(const std::vector<Piece>& from, const std::vector<Piece>& to);

  // The transition at `t`, from 0 (all `from`) to 1 (all `to`); eased here, so
  // the caller's clock can run straight.
  std::vector<Shape> At(float t) const;

  // How many pieces found a partner, and how many went or came unpartnered.
  int Matched() const { return matched_; }
  int Unmatched() const { return static_cast<int>(tracks_.size()) - matched_; }

 private:
  enum class Way { Move, Leave, Arrive };
  // One piece's journey: its contours paired with the partner's, point for
  // point, each pair the same length.
  struct Track {
    Way                  way = Way::Move;
    std::vector<Contour> from, to;
  };
  std::vector<Track> tracks_;
  int                matched_ = 0;
};

}  // namespace sb::morph
