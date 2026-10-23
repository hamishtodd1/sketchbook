// Volumes: the window a function of three variables is seen in.
//
// A function of one variable is a curve, of two a surface, of three this. They
// are separate kinds because each has a different camera, and a display window
// is largely its camera.
//
// There is no surface to stand on and no pixel to put the answer at, so what is
// drawn is what the function *is*: a quantity that is everywhere at once, thick
// where there is a lot of it and clear where there is none. Viridis green where
// it is positive and yellow where it is negative, or hue where the answer is
// complex.
//
// **Perspective, where the surface is orthographic.** A surface is a graph and
// must draw the same quantity the same size wherever it stands. A volume is a
// place you are looking *into*, and there the near things being bigger is
// exactly what says they are near -- take it away and a cloud has no depth in
// it at all. So the wheel moves the eye in and out rather than changing the
// scale, which is what the other window with a perspective camera already does.
//
// It starts turned, where a surface starts flat: looked at square a volume is a
// wash with no depth in it, so there is no discovery to save for later.
#pragma once

#include <algorithm>

#include <memory>
#include <vector>

#include "blocks.h"
#include "dock.h"
#include "fontstore.h"
#include "grip.h"
#include "raylib.h"
#include "surfacedw.h"
#include "types/RealFunctionViz.h"
#include "types/RealViz.h"
#include "viewport.h"

namespace sb {

struct VolumeView {
  // How big it is when it is not docked, as a multiple of where every window of
  // its kind starts. Its grip sets it (grip.h).
  float size = 1.0f;
  CodeBlock* block = nullptr;
  bool       docked = true;
  Vector2    worldPos{};
  // The cube on show, and where the eye is: yaw, pitch, and how far off in
  // units of the cube's half-width.
  Vector2    centre{0.0f, 0.0f};
  float      span = 2.4f;
  // Far enough back that the cube fits corner to corner: at this field of
  // view its diagonal subtends the frame at about four and a half half-widths.
  Vector3    orbit{0.7f, 0.5f, 5.0f};
  bool       selected = false;
  Vector2    dragFrom{};
};

class VolumePanel : public Flanking, public Resizable {
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
  const std::vector<VolumeView>& Views() const { return views_; }
  void Clear() { views_.clear(); dragging_ = false; orbiting_ = false; }
  void Adopt(std::vector<VolumeView> views) { views_ = std::move(views); }

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

  std::vector<VolumeView>      views_;
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
