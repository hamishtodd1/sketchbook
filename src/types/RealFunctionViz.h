// Plots: the window a function is seen in.
//
// A real gets a number line; a function gets one of these. It draws the curve
// from the samples the program printed back, with **input running up the
// vertical axis and output along the horizontal one**. That is the transpose of
// the usual convention, and it is deliberate: these axes are heading towards
// spacetime diagrams, where the vertical axis is time.
//
// Everything else follows the number lines. A plot starts docked under its box
// (below the number lines), can be pulled onto the canvas by its rim, snaps to
// the grid when dropped, zooms on its own wheel, and merges with another plot
// when dropped onto it -- which is how a vector space becomes visible: f, g and
// f + g drawn against one set of axes.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "types/RealViz.h"
#include "blocks.h"
#include "fontstore.h"
#include "raylib.h"
#include "viewport.h"

namespace sb {

struct CurveRef {
  CodeBlock*  block = nullptr;
  std::string name;
};

struct Plot {
  std::vector<CurveRef> refs;
  // The value at the middle of the window: x is output, y is input. Both axes
  // are drawn at the same scale, so a slope of one is a diagonal.
  Vector2               centre{0.0f, 0.0f};
  float                 span = 6.0f;   // input units from the bottom to the top
  bool                  docked = true;
  Vector2               worldPos{};
  CodeBlock*            dock = nullptr;
};

class PlotPanel {
 public:
  using Blocks = std::vector<std::unique_ptr<CodeBlock>>;

  // Plots stack under the number lines, so the panel has to know where those
  // end. Set once; the two panels are siblings, not owners.
  void SetAxes(const AxisPanel* axes) { axes_ = axes; }

  // As AxisPanel::Sync. True if plots came or went.
  bool Sync(const Blocks& blocks);
  void Forget(const CodeBlock* block);

  bool HandleInput(const Viewport& viewport, FontStore& fonts);
  void Draw(const Viewport& viewport, FontStore& fonts) const;

  Rectangle DockedBounds(const CodeBlock* block, const Viewport& viewport,
                         FontStore& fonts) const;

  // As AxisPanel::ActiveVariables.
  void ActiveVariables(const Viewport& viewport, FontStore& fonts,
                       std::vector<std::pair<CodeBlock*, std::string>>* out) const;

  // As AxisPanel::Dragging: a drag in flight gets the pointer first.
  bool Dragging() const { return drag_ != Drag::None; }

  // --- persistence --------------------------------------------------------
  const std::vector<Plot>& Plots() const { return plots_; }
  void Clear() { plots_.clear(); drag_ = Drag::None; }
  void Adopt(std::vector<Plot> plots) { plots_ = std::move(plots); }

 private:
  enum class Drag { None, Pan, Move };

  Rectangle RectFor(std::size_t index, const Viewport& viewport, FontStore& fonts) const;
  const mathprogram::Variable* Resolve(const CurveRef& ref) const;
  // Opens the window out until every curve on it is in view.
  void Reframe(Plot& plot) const;

  std::vector<Plot> plots_;
  const AxisPanel*  axes_ = nullptr;

  Drag        drag_ = Drag::None;
  std::size_t dragPlot_ = 0;
  Vector2     dragGrab_{};
  Vector2     dragScreenPos_{};
  bool        dragMoved_ = false;
};

}  // namespace sb
