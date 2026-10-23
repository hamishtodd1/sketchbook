// The window a colour is seen, and chosen, in.
//
// A `Color` is three numbers, and there is more than one good way to lay three
// numbers out when they are a colour -- so the window has three, and a little
// dropdown beside it, shown while the pointer is on the window, to choose:
//
//   **picker** -- a hue ring with a triangle inside it whose corners are black,
//     white, and the colour on the ring the triangle points at. What drawing
//     programs have used for choosing a colour for decades, and the one for
//     *choosing*: drag round the ring for hue, inside the triangle for the rest.
//   **RGB cube** -- red, green and blue along three axes.
//   **HSV cylinder** -- hue round it, saturation out from the middle, value up.
//
// The cube and the cylinder turn on the right button, like every window with a
// camera, and are drawn orthographic: a colour space is a graph of three
// numbers, not a place to be looked into, so the same step should be the same
// size wherever it is.
//
// **Colour is identity, and here colour is also the value.** So a marker's fill
// is the colour it *is*, and its rim the colour that says *which* variable it
// is, a ring outside a gap. A colour outside 0..1 is drawn where it is, outside
// the solid.
//
// **A colour's marker is a viz**, one per view -- `Viz(Color, "Picker")`,
// `Viz(Color, "Cube")`, `Viz(Color, "Cylinder")` -- so where one goes, how it looks,
// how near the pointer is and where a drag takes it are the viz's to say. What
// stays the window's own is its furniture, and the picker's triangle: the colour
// the triangle points at is shown in it, and moved in it, by the window.
//
// The dropdown is where other choices about this window will go too.
#pragma once

#include <algorithm>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "blocks.h"
#include "dock.h"
#include "fontstore.h"
#include "grip.h"
#include "raylib.h"
#include "viewport.h"
#include "viz.h"

namespace sb {

enum class ColorShape : int { Picker = 0, Cube = 1, Cylinder = 2, kCount = 3 };

struct ColorView {
  // How big it is when it is not docked, as a multiple of where every window of
  // its kind starts. Its grip sets it (grip.h).
  float size = 1.0f;
  CodeBlock*  block = nullptr;
  bool        docked = true;
  Vector2     worldPos{};
  ColorShape  shape = ColorShape::Picker;
  float       yaw = 0.7f;
  float       pitch = 0.45f;
  // The colour the picker's triangle points at. The picker shows one colour's
  // saturation and value at a time; the others sit on the ring at their hue.
  std::string focus;
  // Which way the triangle points while that colour has no hue to say -- a grey
  // has none, and without this the triangle would swing round to red.
  float       focusHue = 0.0f;
  bool        selected = false;
  Vector2     dragFrom{};
  // Which window this is while the app is open, so what it handed its viz can be
  // found again. Not saved.
  mutable int id = 0;
};

class ColorPanel : public Flanking, public Resizable {
 public:
  using Blocks = std::vector<std::unique_ptr<CodeBlock>>;

  void SetBefore(std::vector<const Flanking*> before) { before_ = std::move(before); }
  int  FlankCount(const CodeBlock* block) const override;

  bool Sync(const Blocks& blocks);
  void Forget(const CodeBlock* block);

  bool HandleInput(const Viewport& viewport, FontStore& fonts);
  // The dropdowns. Asked before anything else on the canvas, because one stands
  // outside its window -- over a neighbouring box, as often as not -- and a
  // control that the thing under it answers instead is not a control.
  bool HandleMenus(const Viewport& viewport, FontStore& fonts);
  void Draw(const Viewport& viewport, FontStore& fonts, VizStore& vizes) const;
  // What the windows handed their vizes, on the GPU. By hand.
  void ReleaseGpu();
  // The dropdowns, after every window, since one stands outside its own window
  // and must not be drawn over by a neighbour.
  void DrawChrome(const Viewport& viewport, FontStore& fonts) const;

  Rectangle DockedBounds(const CodeBlock* block, const Viewport& viewport,
                         FontStore& fonts) const;

  // Where a named value's marker is, so that pointing at its name in the code
  // lights it up from the other end, as AxisPanel::MarkerFor does.
  bool MarkerFor(const CodeBlock* block, const std::string& name, const Viewport& viewport,
                 FontStore& fonts, Vector2* out) const;

  // The marker under the pointer, or the one being held: the same set that
  // lights its name in the code and gets a wire.
  // What the rest of the canvas has lit this frame -- a name pointed at in the
  // code, a value held in another window -- so the same value blinks here too.
  void SetLit(std::vector<ActiveValue> lit) { lit_ = std::move(lit); }
  void ActiveVariables(const Viewport& viewport, FontStore& fonts,
                       std::vector<ActiveValue>* out) const;

  // Any gesture in flight owns the pointer until the button comes up.
  bool Dragging() const { return drag_ != Drag::None; }
  bool Carrying() const { return drag_ == Drag::Move && dragMoved_; }
  // The right button turns the camera here, so the canvas must leave its own
  // pan alone for as long as it does.
  bool Orbiting() const { return drag_ == Drag::Orbit; }

  // --- selection ----------------------------------------------------------
  void SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                    FontStore& fonts);
  void Rects(const Viewport& viewport, FontStore& fonts, std::vector<Rectangle>* out) const;
  void ClearSelection();
  bool AnySelected() const;
  bool SelectedRimAt(Vector2 point, const Viewport& viewport, FontStore& fonts) const;
  void BeginSelectionDrag();
  void MoveSelection(Vector2 worldDelta);
  void DeleteSelection();
  void DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const;

  // --- persistence --------------------------------------------------------
  const std::vector<ColorView>& Views() const { return views_; }
  void Clear() { views_.clear(); drag_ = Drag::None; menuOpen_ = -1; }
  void Adopt(std::vector<ColorView> views) { views_ = std::move(views); }

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
  enum class Drag { None, Move, Orbit, Value, Hue };

  // The picker's own marker: the colour its triangle points at, in the triangle.
  struct Marker {
    std::string name;
    Vector2     at{};
    float       rgb[3] = {0.0f, 0.0f, 0.0f};
    bool        free = false;
  };

  Rectangle   RectFor(std::size_t index, const Viewport& viewport, FontStore& fonts) const;
  Rectangle   MenuRect(Rectangle window) const;
  std::string Focus(const ColorView& view) const;
  bool        TriangleMarker(std::size_t index, Rectangle rect, Marker* out) const;

  // --- what each window hands its viz ------------------------------------------
  mutable std::unordered_map<int, VizPane> panes_;
  mutable int                              nextId_ = 1;
  VizPane*    PaneFor(std::size_t index) const;
  // The view's own space -- the plane of the picker's ring, the RGB cube, the
  // HSV cylinder -- to the window.
  VizFrame    FrameFor(std::size_t index, Rectangle rect, Vector2 pointer, float zoom) const;
  Vector2     ScreenOf(std::size_t index, Rectangle rect, Vector3 at) const;
  // The colour whose marker the pointer is on, by its viz's `grab`: the item, or
  // -1. Only values: a sample is pointed at, never taken hold of.
  int         ValueNear(std::size_t index, float zoom) const;
  // Whether the drag in flight is the viz's to answer: anything but the picker's
  // triangle, which is the window's own.
  bool        VizDrags() const;
  bool                HandleMenu(Rectangle window, std::size_t index);
  void                DrawMenu(Rectangle window, std::size_t index, FontStore& fonts) const;

  std::vector<ColorView>        views_;
  std::vector<const Flanking*>  before_;

  Drag        drag_ = Drag::None;
  std::size_t dragView_ = 0;
  Vector2     dragGrab_{};
  Vector2     dragScreenPos_{};
  bool        dragMoved_ = false;
  std::string dragName_;
  // Which window's dropdown is open, or -1.
  int         menuOpen_ = -1;
  std::vector<ActiveValue> lit_;
};

}  // namespace sb
