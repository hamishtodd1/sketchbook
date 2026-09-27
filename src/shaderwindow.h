// The window a shader is seen in.
//
// Pictures: the window a *colour-valued* function of the plane is seen in.
//
// **A box draws because it defines a function**, and how many variables that
// function takes decides which kind of window it gets: one is a curve on a
// plot, two is this, three is a volume. They are separate kinds because each
// has a different camera, and a display window is largely its camera -- this
// one has none at all, because a colour has nowhere to go but the pixel it is
// at and there is nothing to turn.
//
// A *real*-valued function of the plane goes next door to `surfacedw`, where
// the answer has a third direction to use.
//
// One pixel for every input, coloured by what comes out: the colour itself for
// a vec3, hue and lightness for a complex number. There is one of these per box
// rather than per function, and it shows whichever the box is being read
// inside.
//
// It is a window like the others: a rim to carry it by, the grid to land on,
// and the drop target to throw it away on. What it is *not* is a control -- it
// shows what the box makes, and closing it only means you stop seeing that.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "blocks.h"
#include "dock.h"
#include "fontstore.h"
#include "raylib.h"
#include "types/RealFunctionViz.h"
#include "types/RealViz.h"
#include "viewport.h"

namespace sb {

struct ShaderView {
  CodeBlock* block = nullptr;
  bool       docked = true;
  Vector2    worldPos{};
  // The patch of the input plane on show. Its own scale, like every other
  // window's -- the wheel over it zooms the picture rather than the canvas.
  Vector2    centre{0.0f, 0.0f};
  float      span = 4.0f;
  bool       selected = false;
  Vector2    dragFrom{};
};

class ShaderPanel : public Flanking {
 public:
  using Blocks = std::vector<std::unique_ptr<CodeBlock>>;

  // The kinds before this one in the flanking order, so it knows how many slots
  // beside a box are already taken. See dock.h.
  void SetBefore(std::vector<const Flanking*> before) { before_ = std::move(before); }
  int  FlankCount(const CodeBlock* block) const override;

  bool Sync(const Blocks& blocks);
  void Forget(const CodeBlock* block);

  bool HandleInput(const Viewport& viewport, FontStore& fonts);
  void Draw(const Viewport& viewport, FontStore& fonts) const;

  Rectangle DockedBounds(const CodeBlock* block, const Viewport& viewport,
                         FontStore& fonts) const;

  // A picture is a function of the plane, and the pointer over it *is* that
  // function's argument -- `p` in `vec3 ripple(vec2 p)`. It is reported like
  // any other variable, and lights up where it is written inside the function.
  void ActiveVariables(const Viewport& viewport, FontStore& fonts,
                       std::vector<ActiveValue>* out) const;

  bool Dragging() const { return dragging_; }
  // True while a window is actually being carried, which is what brings up the
  // drop target. A press that has not moved yet is not carrying anything.
  bool Carrying() const { return dragging_ && dragMoved_; }

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
  const std::vector<ShaderView>& Views() const { return views_; }
  void Clear() { views_.clear(); dragging_ = false; }
  void Adopt(std::vector<ShaderView> views) { views_ = std::move(views); }

 private:
  Rectangle RectFor(std::size_t index, const Viewport& viewport, FontStore& fonts) const;

  std::vector<ShaderView>       views_;
  std::vector<const Flanking*>  before_;

  bool        dragging_ = false;
  std::size_t dragView_ = 0;
  Vector2     dragGrab_{};
  Vector2     dragScreenPos_{};
  bool        dragMoved_ = false;
};

}  // namespace sb
