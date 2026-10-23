// The two display windows with a camera of their own: space and spacetime.
//
// **A dw has no opinion about what goes in it.** These show every value of a
// box whose type has a viz for them -- `Viz(Tri, "Space")` is the built-in one,
// a point as a ball, and `Viz(Bi, "Space")` written on a canvas puts its
// bivectors in the same window beside them. There are two of them and they
// differ in exactly two things: the algebra a box must be in to have one, and
// the sky they draw. Cl(3,0,1) is space and Cl(2,1,1) is spacetime, where the
// third axis is time.
//
// Like the number line, **a box gets one and all of its values share it**: a
// stack of one-value windows would say nothing about how the values compare,
// and the values in a box are read together.
//
// Everything else on this canvas is drawn in screen space. This is the one
// window with a camera of its own, so it is rendered into a texture and the
// texture is what lands on the canvas -- which keeps the depth buffer and the
// 3D viewport to itself and leaves the rest of the app exactly as it was.
#pragma once

#include <algorithm>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "blocks.h"
#include "dock.h"
#include "fontstore.h"
#include "glext.h"
#include "grip.h"
#include "raylib.h"
#include "touch.h"
#include "viewport.h"
#include "viz.h"

namespace sb {

// Which of the two this is. They share everything but the algebra they take
// their points from and the sky they hang behind them.
enum class SpaceKind { Space, Spacetime };

// The algebra each one shows. A box working in neither gets no window.
clifford::Signature SignatureOf(SpaceKind kind);
SpaceKind           KindOfSignature(const clifford::Signature& sig, bool* found);

// One value being shown: which box it belongs to and what it is called there.
struct PointRef {
  CodeBlock*  block = nullptr;
  std::string name;
};

struct SpaceWindow {
  // How big it is when it is not docked, as a multiple of where every window of
  // its kind starts. Its grip sets it (grip.h).
  float size = 1.0f;
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

  // Which window this is for as long as the app is open, so what it has on the
  // GPU can be found again after the list is reordered. Not saved.
  int id = 0;
  // Worked out by every Sync, and not saved: the types in this window that have
  // a viz here, each drawn by its own; and whether it is showing the spread a
  // function of its box is being fed.
  std::vector<std::string> types;
  bool                     feeding = false;
};

class SpacePanel : public Flanking, public Resizable {
 public:
  using Blocks = std::vector<std::unique_ptr<CodeBlock>>;

  // What a viz is told when it chooses an example: where the walls are, which
  // are the same in every space window.
  static VizFrame ExampleFrame();

  ~SpacePanel() override;

  // The kinds before this one in the flanking order, so it knows how many slots
  // beside a box are already taken. See dock.h.
  void SetBefore(std::vector<const Flanking*> before) { before_ = std::move(before); }
  int  FlankCount(const CodeBlock* block) const override;


  // A destructor cannot free what is on the GPU, so whoever destroys this
  // releases it first, by hand.
  void ReleaseGpu();

  // Which values go where. A value goes in if its type has a viz for this kind
  // of window -- which `vizes` knows without compiling one.
  bool Sync(const Blocks& blocks, const VizStore& vizes);
  void Forget(const CodeBlock* block);

  bool HandleInput(const Viewport& viewport, FontStore& fonts);
  // A point is drawn by the viz for its type in this window, from `vizes`.
  void Draw(const Viewport& viewport, FontStore& fonts, VizStore& vizes);

  // Where a named value's marker is, so that pointing at its name in the code
  // lights it up from the other end, as AxisPanel::MarkerFor does.
  bool MarkerFor(const CodeBlock* block, const std::string& name, const Viewport& viewport,
                 FontStore& fonts, Vector2* out) const;

  // The point under the pointer, and the one being dragged: what lights up,
  // and where its wire leaves from -- the viz's `position`.
  // What the rest of the canvas has lit this frame -- a name pointed at in the
  // code, a value held in another window -- so the same value blinks here too.
  void SetLit(std::vector<ActiveValue> lit) { lit_ = std::move(lit); }
  void ActiveVariables(const Viewport& viewport, FontStore& fonts,
                       std::vector<ActiveValue>* out) const;

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

  // Carrying a window, or dragging a point in one.
  bool Dragging() const { return dragging_ || held_.active; }
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
  const std::vector<SpaceWindow>& Windows() const { return windows_; }
  void Clear();
  void Adopt(std::vector<SpaceWindow> windows);

  // The grip (grip.h).
  int       GripCount() const override { return static_cast<int>(windows_.size()); }
  bool      WindowDocked(int index) const override { return windows_[static_cast<std::size_t>(index)].docked; }
  Vector2   WindowWorldPos(int index) const override { return windows_[static_cast<std::size_t>(index)].worldPos; }
  void      SetWindowWorldPos(int index, Vector2 at) override { windows_[static_cast<std::size_t>(index)].worldPos = at; }
  Rectangle GripWindow(int index, const Viewport& viewport, FontStore& fonts) const override {
    return RectFor(static_cast<std::size_t>(index), viewport, fonts);
  }
  void Loosen(int index, const Viewport& viewport, FontStore& fonts) override {
    LoosenWindow(windows_[static_cast<std::size_t>(index)], GripWindow(index, viewport, fonts),
                 viewport);
  }
  float WindowSize(int index) const override { return windows_[static_cast<std::size_t>(index)].size; }
  void  SetWindowSize(int index, float size) override {
    windows_[static_cast<std::size_t>(index)].size = std::clamp(size, kMinWindowSize, kMaxWindowSize);
  }

 private:
  Rectangle RectFor(std::size_t index, const Viewport& viewport, FontStore& fonts) const;
  // Where a point in the space lands on screen. The same projection the drawing
  // used, carried through the blit that puts the scene on the canvas.
  bool      Project(std::size_t index, Vector3 at, const Viewport& viewport, FontStore& fonts,
                    Vector2* out) const;
  void      EnsureGpu();

  // What each window has handed each type's viz, and heard back, by the
  // window's id and the type. One pane per type, since one viz draws one type.
  VizPane*       PaneFor(std::size_t index, const std::string& type);
  const VizPane* PaneFor(std::size_t index, const std::string& type) const;
  // One item in one of a window's panes.
  struct Hit {
    std::string type;
    int         item = -1;
  };
  const VizItem* ItemOf(std::size_t index, const Hit& hit) const;
  VizFrame       FrameFor(std::size_t index, Rectangle rect, Vector2 pointer) const;
  // Hands a window's values of one type to that type's viz and asks it where
  // they are. `lit` is the one that blinks.
  void           Prepare(std::size_t index, Rectangle rect, const std::string& type,
                         const VizGpu* viz, const VizItem& lit);
  // The value, or sample, whose viz the pointer is nearest by the viz's own
  // `grab`, within `reach` pixels, across every type in the window.
  Hit            Nearest(std::size_t index, bool samples, float reach) const;
  // Where a value is in a window's panes, if it is.
  Hit            Find(std::size_t index, const CodeBlock* block, const std::string& name) const;
  void        DrawScene(std::size_t index, Rectangle rect,
                        const std::vector<std::pair<std::string, const VizGpu*>>& vizes);

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

  // A value being dragged, by its viz's `drag`.
  struct Held {
    bool        active = false;
    int         window = 0;       // its window's id
    CodeBlock*  block = nullptr;
    std::string name;
    std::string type;             // whose viz it is
  };
  Held held_;

  std::map<std::pair<int, std::string>, VizPane> panes_;
  int                           nextId_ = 1;
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
  Model           wall_{};
  int             limitsMinLoc_ = -1;
  int             limitsMaxLoc_ = -1;
  int             atInfinityLoc_ = -1;
  int             cullLoc_ = -1;
  std::vector<ActiveValue> lit_;
};

}  // namespace sb
