// Animating text from one writing to another -- code into its LaTeX, or one
// formula into another -- for presenting and for making videos, the way Manim
// does.
//
// Nothing is kept ready to animate: a transition is made when one is wanted, from
// the outlines of what is on each side (morph.h does the moving). This file
// makes those outlines from code and from a TeX layout, fills them on the GPU,
// and runs `--animate_text_transition`, a window that shows one transition back
// and forth and closes.
#pragma once

#include <string>
#include <vector>

#include "fontstore.h"
#include "morph.h"
#include "raylib.h"
#include "tex/tex.h"

namespace sb::transition {

// A line of code as pieces: an identifier is one piece keyed by its name, and
// any other character is one keyed by what it reads as (`*` as a dot, `/` as a
// fraction's rule). `pen` is the left end of the baseline.
std::vector<morph::Piece> CodePieces(FontStore& fonts, const std::string& code, float emPx,
                                     Vector2 pen);

// A laid-out formula as pieces: everything a `\sbref` mark covers is one piece
// keyed by the name `marks` gives it, a run of upright letters (`\sin`) one
// keyed by the word, and anything else one per glyph or rule.
std::vector<morph::Piece> LatexPieces(FontStore& fonts, const tex::Layout& layout,
                                      const std::vector<std::string>& marks, Vector2 origin);

// Fills outlines by the non-zero rule, smoothly: each shape's points go to the
// GPU in a float texture, and a fragment shader over its bounding box finds the
// winding and the distance to the nearest edge, which is the coverage.
class OutlineFill {
 public:
  OutlineFill() = default;
  ~OutlineFill() { Unload(); }
  OutlineFill(const OutlineFill&) = delete;
  OutlineFill& operator=(const OutlineFill&) = delete;

  bool Load();
  void Unload();
  void Draw(const std::vector<morph::Shape>& shapes, Color ink);

 private:
  Shader             shader_{};
  Texture2D          points_{};
  int                rowLoc_ = -1, countLoc_ = -1, pointsLoc_ = -1;
  std::vector<float> buffer_;
};

// The window `--animate_text_transition` opens instead of the canvas. One side
// is a line of code, shown turning into its LaTeX and back; two are formulas --
// each a line of code read as LaTeX, or `$...$` for LaTeX itself -- shown turning
// into each other. Three times there and back, then it closes. A `screenshot`
// is taken at frame `frames`, the clock stepping 1/60 s a frame so a capture
// lands where it is asked to, and saved by `saveFrame`. Returns the process's
// exit code.
int Run(const std::string& assetDir, const std::vector<std::string>& sides,
        const std::string& screenshot, int frames, void (*saveFrame)(const std::string&));

}  // namespace sb::transition
