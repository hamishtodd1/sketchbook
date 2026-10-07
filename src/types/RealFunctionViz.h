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
  // Which curve is under a screen point. A curve is drawn by the GPU and nothing
  // on this side knows where one is, so each curve's viz is asked, by its
  // `grab`, how near the point is to it, and the nearest within reach is the
  // one. A read back from the GPU for each curve, once, when the press lands.
  enum class Pick { Idle, Pending, Found, None };
  // The plot under a point, not its rim. With one curve on it the answer is
  // already known and `curve` is filled; with more it has to be asked.
  bool PlotUnder(Vector2 point, const Viewport& viewport, FontStore& fonts, std::size_t* plot,
                 Rectangle* window, CurveRef* curve) const;
  void AskCurveAt(std::size_t plot, Vector2 point, const Viewport& viewport, FontStore& fonts);
  Pick CurveAnswer(CurveRef* out) const;
  void ForgetPick() { pick_ = Pick::Idle; }
  // Takes a curve off its plot and starts carrying it as a plot of its own, from
  // the pointer, at the scale it was being seen at. The drop is any plot's.
  bool BeginPullOut(CodeBlock* block, const std::string& name, Vector2 pointer,
                    const Viewport& viewport);

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
  void Draw(const Viewport& viewport, FontStore& fonts) const;

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
  // Where each of its windows is on screen.
  void Rects(const Viewport& viewport, FontStore& fonts, std::vector<Rectangle>* out) const;
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
  // One curve of a plot, however the canvas says curves are drawn.
  void DrawCurve(const Plot& plot, std::size_t c, Rectangle rect, float scale, float thickness,
                 Color tint) const;

  std::vector<Plot> plots_;
  mutable Pick        pick_ = Pick::Idle;
  mutable CurveRef    pickRef_;
  const AxisPanel*  axes_ = nullptr;

  Drag        drag_ = Drag::None;
  std::size_t dragPlot_ = 0;
  Vector2     dragGrab_{};
  Vector2     dragScreenPos_{};
  bool        dragMoved_ = false;
};

}  // namespace sb
