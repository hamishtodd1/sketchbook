// The window a shader is seen in.
//
// A box whose code turned out to be GLSL draws something, and this is where it
// is drawn. Unlike a number line or a plot it stands for a whole box rather
// than for one variable, so there is one of these per shader box.
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
  bool       selected = false;
  Vector2    dragFrom{};
};

class ShaderPanel {
 public:
  using Blocks = std::vector<std::unique_ptr<CodeBlock>>;

  bool Sync(const Blocks& blocks);
  void Forget(const CodeBlock* block);

  bool HandleInput(const Viewport& viewport, FontStore& fonts);
  void Draw(const Viewport& viewport, FontStore& fonts) const;

  Rectangle DockedBounds(const CodeBlock* block, const Viewport& viewport,
                         FontStore& fonts) const;

  // Hovering the picture *is* setting the shader's input, so the pointer being
  // over one of these means `fragTexCoord` is the thing under the cursor. It is
  // reported like any other variable, and lights up where it is written.
  void ActiveVariables(const Viewport& viewport, FontStore& fonts,
                       std::vector<std::pair<CodeBlock*, std::string>>* out) const;

  bool Dragging() const { return dragging_; }
  // True while a window is actually being carried, which is what brings up the
  // drop target. A press that has not moved yet is not carrying anything.
  bool Carrying() const { return dragging_ && dragMoved_; }

  // --- selection ----------------------------------------------------------
  // The lasso catches windows by their middle. A selected one wears its rim,
  // moves with the rest of the selection, and goes with it.
  void SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                    FontStore& fonts);
  void ClearSelection();
  bool AnySelected() const;
  bool SelectedRimAt(Vector2 point, const Viewport& viewport, FontStore& fonts) const;
  // Remembers where everything selected is, so a drag can place it relative to
  // there rather than accumulating a delta frame by frame.
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

  std::vector<ShaderView> views_;

  bool        dragging_ = false;
  std::size_t dragView_ = 0;
  Vector2     dragGrab_{};
  Vector2     dragScreenPos_{};
  bool        dragMoved_ = false;
};

}  // namespace sb
