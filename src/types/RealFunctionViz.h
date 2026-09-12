// Plots: the window a function is seen in.
//
// A real gets a number line; a function gets one of these. The curve is drawn
// by calling the function per pixel, with **input running up the vertical axis
// and output along the horizontal one**. That is the transpose of
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
#include "viz.h"
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
  bool                  selected = false;
  Vector2               dragFrom{};
};

class PlotPanel {
 public:
  using Blocks = std::vector<std::unique_ptr<CodeBlock>>;

  // Plots stack under the number lines, so the panel has to know where those
  // end. Set once; the two panels are siblings, not owners.
  void SetAxes(const AxisPanel* axes) { axes_ = axes; }

  // As AxisPanel::Sync. True if plots came or went.
  bool Sync(const Blocks& blocks);

  // A curve is drawn by the box's own shader now, so this panel owns a texture.
  // A destructor cannot free what is on the GPU, so it is released by hand.
  void ReleaseGpu();
  void Forget(const CodeBlock* block);

  bool HandleInput(const Viewport& viewport, FontStore& fonts);
  // `vizes` is how the canvas says a function should be drawn, if it says
  // anything.
  void Draw(const Viewport& viewport, FontStore& fonts,
            const VizStore* vizes = nullptr) const;

  Rectangle DockedBounds(const CodeBlock* block, const Viewport& viewport,
                         FontStore& fonts) const;

  // As AxisPanel::ValueAt. A plot stands for every curve on it, so what a finger
  // has landed on is the first of them -- there is nothing finer to point at.
  bool ValueAt(Vector2 point, const Viewport& viewport, FontStore& fonts,
               CurveRef* out) const;

  // As AxisPanel::MarkerFor: where a named curve is, so that taking hold of its
  // name in the text can light up the same things.
  bool MarkerFor(const CodeBlock* block, const std::string& name, const Viewport& viewport,
                 FontStore& fonts, Vector2* out) const;

  // As AxisPanel::ActiveVariables.
  void ActiveVariables(const Viewport& viewport, FontStore& fonts,
                       std::vector<ActiveValue>* out) const;

  // As AxisPanel::Dragging: a drag in flight gets the pointer first.
  bool Dragging() const { return drag_ != Drag::None; }
  // As AxisPanel::Carrying.
  bool Carrying() const { return drag_ == Drag::Move && dragMoved_; }

  // --- selection ----------------------------------------------------------
  // As every other window: the lasso catches by the middle, a selected one
  // wears its rim, and the whole selection moves and goes as one. The bodies
  // are the shared ones in selection.h; what differs is what closing one means.
  void SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                    FontStore& fonts);
  void ClearSelection();
  bool AnySelected() const;
  bool SelectedRimAt(Vector2 point, const Viewport& viewport, FontStore& fonts) const;
  void BeginSelectionDrag();
  void MoveSelection(Vector2 worldDelta);
  void DeleteSelection();
  void DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const;


  // --- persistence --------------------------------------------------------
  const std::vector<Plot>& Plots() const { return plots_; }
  void Clear() { plots_.clear(); drag_ = Drag::None; }
  void Adopt(std::vector<Plot> plots) { plots_ = std::move(plots); }

 private:
  enum class Drag { None, Pan, Move };

  void      EnsureCurveSurface() const;
  mutable bool      haveCurveSurface_ = false;
  mutable Texture2D curveSurface_{};

  Rectangle RectFor(std::size_t index, const Viewport& viewport, FontStore& fonts) const;
  // As AxisPanel::Shown: a box being read a line at a time only talks about the
  // variables on that line, and the windows for the rest of it stand down.
  static bool Shown(const CurveRef& ref);
  static bool Shown(const Plot& plot);
  const mathprogram::Variable* Resolve(const CurveRef& ref) const;

  std::vector<Plot> plots_;
  const AxisPanel*  axes_ = nullptr;

  Drag        drag_ = Drag::None;
  std::size_t dragPlot_ = 0;
  Vector2     dragGrab_{};
  Vector2     dragScreenPos_{};
  bool        dragMoved_ = false;
};

}  // namespace sb
