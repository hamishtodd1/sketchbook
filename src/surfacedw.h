// Surfaces: the window a real-valued function of the plane is seen in.
//
// **How many variables a function takes decides which window it gets.** One is
// a curve on a plot; two is this; three is a volume. They are separate kinds
// because each has a different camera, and a display window is largely its
// camera: a plot has none, this has an orthographic one, a volume has a
// perspective one.
//
// It opens looking straight down, where it is exactly the flat viridis picture
// the same function would have made -- and the right button turns it, and what
// looked like a texture turns out to have been a topographic map seen from
// above. That is a thing you find out rather than a thing you are told, which
// is the whole reason the third direction is there rather than thrown away.
//
// **Orthographic, where the volume is perspective.** A surface is a graph, and
// a graph must draw the same quantity the same size wherever it stands.
//
// Everything else follows the plots: docked under its box below the number
// lines and the plots, carried by its rim, snapping to the grid, caught by the
// lasso, thrown away on the cross.
#pragma once

#include <algorithm>

#include <memory>
#include <vector>

#include "blocks.h"
#include "dock.h"
#include "fontstore.h"
#include "grip.h"
#include "raylib.h"
#include "types/RealFunctionViz.h"
#include "types/RealViz.h"
#include "viewport.h"

namespace sb {

struct SurfaceView {
  // How big it is when it is not docked, as a multiple of where every window of
  // its kind starts. Its grip sets it (grip.h).
  float size = 1.0f;
  CodeBlock* block = nullptr;
  bool       docked = true;
  Vector2    worldPos{};
  // The patch of the plane on show, and where the eye is over it: yaw, pitch
  // above the plane, and a distance the orthographic camera has no use for but
  // which is kept so both kinds of window save the same three numbers.
  Vector2    centre{0.0f, 0.0f};
  float      span = 4.0f;
  Vector3    orbit{0.0f, 1.5707963f, 3.0f};
  bool       selected = false;
  Vector2    dragFrom{};
};

class SurfacePanel : public Flanking, public Resizable {
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

  // True while the eye is being moved, so the canvas leaves its own
  // right-button pan alone for as long as it lasts.
  bool Orbiting() const { return orbiting_; }
  bool Dragging() const { return dragging_; }
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
  const std::vector<SurfaceView>& Views() const { return views_; }
  void Clear() { views_.clear(); dragging_ = false; orbiting_ = false; }
  void Adopt(std::vector<SurfaceView> views) { views_ = std::move(views); }

  // The grip (grip.h).
  int       GripCount() const override { return static_cast<int>(views_.size()); }
  bool      WindowDocked(int index) const override { return views_[static_cast<std::size_t>(index)].docked; }
  Vector2   WindowWorldPos(int index) const override { return views_[static_cast<std::size_t>(index)].worldPos; }
  void      SetWindowWorldPos(int index, Vector2 at) override { views_[static_cast<std::size_t>(index)].worldPos = at; }
  Rectangle GripWindow(int index, const Viewport& viewport, FontStore& fonts) const override {
    return RectFor(static_cast<std::size_t>(index), viewport, fonts);
  }
  void Loosen(int index, const Viewport& viewport, FontStore& fonts) override {
    LoosenWindow(views_[static_cast<std::size_t>(index)], GripWindow(index, viewport, fonts),
                 viewport);
  }
  float WindowSize(int index) const override { return views_[static_cast<std::size_t>(index)].size; }
  void  SetWindowSize(int index, float size) override {
    views_[static_cast<std::size_t>(index)].size = std::clamp(size, kMinWindowSize, kMaxWindowSize);
  }

 private:
  Rectangle RectFor(std::size_t index, const Viewport& viewport, FontStore& fonts) const;

  std::vector<SurfaceView>     views_;
  std::vector<const Flanking*> before_;

  bool        dragging_ = false;
  std::size_t dragView_ = 0;
  Vector2     dragGrab_{};
  Vector2     dragScreenPos_{};
  bool        dragMoved_ = false;

  bool        orbiting_ = false;
  std::size_t orbitView_ = 0;
};

}  // namespace sb
