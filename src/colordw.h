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
// The dropdown is where other choices about this window will go too.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "blocks.h"
#include "dock.h"
#include "fontstore.h"
#include "raylib.h"
#include "viewport.h"

namespace sb {

enum class ColorShape : int { Picker = 0, Cube = 1, Cylinder = 2, kCount = 3 };

struct ColorView {
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
};

class ColorPanel : public Flanking {
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
  void Draw(const Viewport& viewport, FontStore& fonts) const;
  // The dropdowns, after every window, since one stands outside its own window
  // and must not be drawn over by a neighbour.
  void DrawChrome(const Viewport& viewport, FontStore& fonts) const;

  Rectangle DockedBounds(const CodeBlock* block, const Viewport& viewport,
                         FontStore& fonts) const;

  // The marker under the pointer, or the one being held: the same set that
  // lights its name in the code and gets a wire.
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

 private:
  enum class Drag { None, Move, Orbit, Value, Hue };

  struct Marker {
    std::string name;
    Vector2     at{};
    float       rgb[3] = {0.0f, 0.0f, 0.0f};
    bool        free = false;
    bool        onRing = false;   // the picker's hue marker, not its triangle one
  };

  Rectangle           RectFor(std::size_t index, const Viewport& viewport,
                              FontStore& fonts) const;
  Rectangle           MenuRect(Rectangle window) const;
  std::vector<Marker> Markers(std::size_t index, Rectangle rect) const;
  std::string         Focus(const ColorView& view) const;
  // The marker a press at `point` would take hold of, or -1.
  int                 MarkerAt(const std::vector<Marker>& markers, Vector2 point,
                               float zoom) const;
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
  float       dragDepth_ = 0.0f;
  // Which window's dropdown is open, or -1.
  int         menuOpen_ = -1;
};

}  // namespace sb
