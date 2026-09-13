// The two display windows that show points, in space and in spacetime.
//
// A number line shows a real and a plot shows a function; these show a *point*,
// which is what a trivector of a 4D projective algebra is. There are two of
// them and they differ in exactly two things: the algebra whose `Tri` they
// accept, and the sky they draw. Cl(3,0,1) is space and Cl(2,1,1) is spacetime,
// where the third axis is time.
//
// Like the number line, **a box gets one and all of its points share it**: a
// stack of one-point windows would say nothing about how the points compare,
// and the points in a box are read together.
//
// Everything else on this canvas is drawn in screen space. This is the one
// window with a camera of its own, so it is rendered into a texture and the
// texture is what lands on the canvas -- which keeps the depth buffer and the
// 3D viewport to itself and leaves the rest of the app exactly as it was.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "blocks.h"
#include "dock.h"
#include "fontstore.h"
#include "glext.h"
#include "raylib.h"
#include "touch.h"
#include "viewport.h"

namespace sb {

// Which of the two this is. They share everything but the algebra they take
// their points from and the sky they hang behind them.
enum class SpaceKind { Space, Spacetime };

// The algebra each one shows. A box working in neither gets no window.
clifford::Signature SignatureOf(SpaceKind kind);
SpaceKind           KindOfSignature(const clifford::Signature& sig, bool* found);

// One point being shown: which box it belongs to and what it is called there.
struct PointRef {
  CodeBlock*  block = nullptr;
  std::string name;
};

struct SpaceWindow {
  SpaceKind             kind = SpaceKind::Space;
  bool                  docked = true;
  Vector2               worldPos{};
  std::vector<PointRef> refs;
  CodeBlock*            dock = nullptr;
  // The box's own window: where a point nobody has moved lives, and where the
  // spread a function of the box is being fed is shown. A window a point was
  // pulled out into is not one, and neither gets new points nor shows a spread.
  bool                  home = true;

  // Where the camera is, as an orbit about the origin. Saved with the canvas,
  // since where you were looking from is part of what you set up.
  float yaw = 1.09f;
  float pitch = -0.25f;
  float dist = 16.0f;

  // Whether things outside the cuboid are cut away. The cuboid is a window into
  // the space rather than a box containing it, so what falls outside it is
  // normally not drawn -- but a point at infinity is nowhere in particular and
  // cutting it would be cutting the very thing worth seeing.
  bool cullOutside = true;

  bool    selected = false;
  Vector2 dragFrom{};
};

class SpacePanel : public Flanking {
 public:
  using Blocks = std::vector<std::unique_ptr<CodeBlock>>;

  ~SpacePanel() override;

  // The kinds before this one in the flanking order, so it knows how many slots
  // beside a box are already taken. See dock.h.
  void SetBefore(std::vector<const Flanking*> before) { before_ = std::move(before); }
  int  FlankCount(const CodeBlock* block) const override;


  // A destructor cannot free what is on the GPU, so whoever destroys this
  // releases it first, by hand.
  void ReleaseGpu();

  bool Sync(const Blocks& blocks);
  void Forget(const CodeBlock* block);

  bool HandleInput(const Viewport& viewport, FontStore& fonts);
  void Draw(const Viewport& viewport, FontStore& fonts);

  Rectangle DockedBounds(const CodeBlock* block, const Viewport& viewport,
                         FontStore& fonts) const;

  // True while the pointer is orbiting one of these, so the canvas leaves its
  // own right-button pan alone for as long as it lasts -- the same courtesy a
  // number line's scroll already gets.
  bool Orbiting() const { return orbiting_; }

  // Where the pointer is on one of a fed function's samples in one of these
  // windows, if it is. A point in space is the answer to a question asked
  // somewhere else, and pointing at it is one way of asking which question.
  bool SampleUnder(Vector2 point, const Viewport& viewport, FontStore& fonts,
                   const CodeBlock** block, int* sample) const;

  // The point under a screen point, and the window it is in. Not the rim.
  bool VizUnder(Vector2 point, const Viewport& viewport, FontStore& fonts, PointRef* out,
                Rectangle* window) const;
  // Takes a point out of whatever window it is in and starts carrying it as a
  // window of its own, from the pointer -- looking at it from where the window
  // it left was looking, so it does not jump. The drop is any carried window's.
  bool BeginPullOut(CodeBlock* block, const std::string& name, Vector2 pointer,
                    const Viewport& viewport);
  // Where one end of a lit sample is on screen, so a wire can be drawn to it.
  bool SampleMarker(const CodeBlock* block, int sample, bool output, const Viewport& viewport,
                    FontStore& fonts, Vector2* out) const;

  bool Dragging() const { return dragging_; }
  bool Carrying() const { return dragging_ && dragMoved_; }

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
  const std::vector<SpaceWindow>& Windows() const { return windows_; }
  void Clear();
  void Adopt(std::vector<SpaceWindow> windows);

 private:
  Rectangle RectFor(std::size_t index, const Viewport& viewport, FontStore& fonts) const;
  // Where a point in the space lands on screen. The same projection the drawing
  // used, carried through the blit that puts the scene on the canvas.
  bool      Project(std::size_t index, Vector3 at, const Viewport& viewport, FontStore& fonts,
                    Vector2* out) const;
  void      EnsureGpu();
  void      DrawScene(const SpaceWindow& window, Rectangle rect);

  std::vector<SpaceWindow>     windows_;
  std::vector<const Flanking*> before_;

  bool        dragging_ = false;
  std::size_t dragView_ = 0;
  Vector2     dragGrab_{};
  Vector2     dragScreenPos_{};
  bool        dragMoved_ = false;

  bool        orbiting_ = false;
  std::size_t orbitView_ = 0;
  Vector2     orbitFrom_{};
  // The same gesture with two fingers, which is what a tablet has instead of a
  // right button. It is tracked separately because it ends by a finger lifting
  // rather than by a button coming up.
  bool        touchOrbit_ = false;

  // --- the GPU side -------------------------------------------------------
  //
  // One render target between every one of these windows, not one each: it is
  // drawn into and blitted immediately, so a second window can have it back the
  // moment the first is done with it. That is what keeps a canvas full of them
  // from being a canvas full of framebuffers.
  bool            haveGpu_ = false;
  glext::MultisampleTarget msaa_;
  RenderTexture2D target_{};
  // The shared target, 16:9 like the windows drawn into it, so the projection
  // it gives the camera is already the right shape.
  int             targetWidth_ = 0;
  int             targetHeight_ = 0;
  Shader          sky_[2]{};
  Shader          solid_{};
  Model           skyBall_{};
  Model           ball_{};
  Model           wall_{};
  int             limitsMinLoc_ = -1;
  int             limitsMaxLoc_ = -1;
  int             atInfinityLoc_ = -1;
  int             cullLoc_ = -1;
};

}  // namespace sb
