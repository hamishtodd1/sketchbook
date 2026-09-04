// Number lines: the knobs that choose values.
//
// **A box gets one.** All of its reals share it, on one scale framed so that
// every one of them is in view -- because the values in a box are read
// together, and a stack of one-value axes says nothing about how they compare.
// Free variables get a handle you can drag; derived ones get a read-only marker
// that moves when its inputs do.
//
// A variable that deserves its own axis is *pulled out* of the box: select part
// of where it is written and drag it onto the canvas. Lines can also be dropped
// onto one another to share a single axis -- which is a way of *seeing* values
// together, not a claim that they are equal.
//
// A line refers to variables by (box, name), so an axis can carry values from
// two different boxes. The panel therefore belongs to the canvas, not to any
// one box.
//
// Each line zooms and pans on its own, independently of the canvas.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "blocks.h"
#include "fontstore.h"
#include "raylib.h"
#include "viewport.h"
#include "viz.h"

namespace sb {

struct AxisRef {
  CodeBlock*  block = nullptr;
  std::string name;
};

struct NumberLine {
  std::vector<AxisRef> refs;
  float                centre = 0.0f;
  float                span = 4.0f;   // values across the full width
  // An axis frames itself once, when the values it carries first exist. Not
  // saved: a canvas that was written down keeps the scale it was left at.
  bool                 framed = false;
  bool                 docked = true;
  Vector2              worldPos{};    // only meaningful once undocked
  // Which box a docked line stacks under. Stays put when lines merge.
  CodeBlock*           dock = nullptr;
  bool                 selected = false;
  Vector2              dragFrom{};   // where it was when a selection drag began
};

class AxisPanel {
 public:
  using Blocks = std::vector<std::unique_ptr<CodeBlock>>;

  // Brings the set of lines into step with every box's variables, keeping
  // whatever arrangement already exists for the ones that survived. Returns
  // true if lines came or went, which changes how much room a box takes up.
  bool Sync(const Blocks& blocks);
  // Drops everything belonging to a box that is going away.
  void Forget(const CodeBlock* block);

  // Returns true if it took the pointer or the wheel, so the canvas can stay
  // put while an axis is being used.
  bool HandleInput(const Viewport& viewport, FontStore& fonts, const Blocks& blocks);
  // `vizes` is how the canvas says a real should be drawn, if it says anything.
  // Null, or silent about `float`, and the marker below is what you get -- the
  // one the app was born with.
  void Draw(const Viewport& viewport, FontStore& fonts, const VizStore* vizes = nullptr) const;

  // The variables in use: the one whose handle is being dragged, everything on
  // a row being carried, and -- while merely hovering -- the single handle a
  // press would take hold of, if there is one.
  //
  // That last one is the rule worth stating: what lights up under the pointer
  // is a **promise about the press**. Lighting every value on the row would
  // promise all of them when the press is about to be about one, and over the
  // bare axis it would promise a value where a press makes a new one instead.
  void ActiveVariables(const Viewport& viewport, FontStore& fonts,
                       std::vector<ActiveValue>* out) const;

  // The screen rectangle the rows docked under a box cover, or a zero-size one
  // if nothing is docked there. They travel with the box, so as far as the
  // canvas is concerned they are part of it.
  Rectangle DockedBounds(const CodeBlock* block, const Viewport& viewport,
                         FontStore& fonts) const;

  // The box a click on the axis under the pointer would write a value into, or
  // nullptr if the pointer is not making that offer -- because it is over a
  // marker, over the rim, or not over an axis at all. The box answers by
  // blinking a caret where the declaration would go.
  CodeBlock* InsertTarget(const Viewport& viewport, FontStore& fonts) const;

  // True while an axis is being scrolled on the right button, so the canvas
  // leaves its own right-button pan alone for as long as that lasts.
  bool Scrolling() const { return drag_ == Drag::Pan; }

  // The value a point on screen is on, if it is on one. Used by the gesture
  // that takes hold of two values at once, which has to know what a finger has
  // landed on before it can freeze it.
  bool ValueAt(Vector2 point, const Viewport& viewport, FontStore& fonts, AxisRef* out) const;

  // Gives up whatever drag is running and puts back the value it had changed.
  // A handle taken hold of by one finger has not been *chosen* yet, so when a
  // second finger arrives and the two together mean something else, the first
  // one's effect has to be undone rather than kept.
  void AbandonDrag();

  // Where a named variable's marker is, if this panel is showing one. Taking
  // hold of a name in the text lights up the same things as taking hold of its
  // window, and this is what says where the other end of the wire goes.
  bool MarkerFor(const CodeBlock* block, const std::string& name, const Viewport& viewport,
                 FontStore& fonts, Vector2* out) const;

  // Takes a variable off the axis it shares with the rest of its box and starts
  // carrying it as an axis of its own, from the pointer. The drop is the same
  // drop any other carried row gets -- onto the canvas, onto another axis to
  // merge, or onto the cross.
  bool BeginPullOut(CodeBlock* block, const std::string& name, Vector2 pointer,
                    const Viewport& viewport);

  // What a ref currently stands for, or null. Public because the nearest-marker
  // search is a free function shared by two callers.
  const mathprogram::Variable* Value(const AxisRef& ref) const { return Resolve(ref); }

  // Whether a value still has anything to say -- see the private half below.
  static bool Shown(const AxisRef& ref);

  bool Empty() const { return lines_.empty(); }
  // True while a row is being dragged. A drag in flight has to be offered the
  // pointer before anything else, or letting go over another window's rim would
  // leave the row stuck to the cursor.
  bool Dragging() const { return drag_ != Drag::None; }
  // True while a row is actually being carried by its rim, which is what brings
  // up the drop target. A press that has not moved is not carrying anything.
  bool Carrying() const { return drag_ == Drag::Move && dragMoved_; }

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
  const std::vector<NumberLine>& Lines() const { return lines_; }
  void Clear() { lines_.clear(); drag_ = Drag::None; dragRef_ = {}; }
  void Adopt(std::vector<NumberLine> lines) {
    lines_ = std::move(lines);
    // A canvas that was written down keeps the scale it was saved at.
    for (NumberLine& line : lines_) line.framed = true;
  }

 private:
  enum class Drag { None, Handle, Pan, Move };

  Rectangle RectFor(std::size_t index, const Viewport& viewport, FontStore& fonts) const;
  static Rectangle AxisArea(Rectangle row);
  // Whether a value still has anything to say. A box being read a line at a
  // time only talks about the variables written on that line, and the windows
  // for the rest of it stand down -- not drawn, and taking no pointer. Their
  // *place* in the stack is kept, so nothing shuffles as the caret moves.
  static bool Shown(const NumberLine& line);
  // The handle a left press would take hold of on this row: the nearest free
  // marker within grabbing distance, which beats the rim and the scroll alike.
  // One function, used both to decide what a press grabs and to decide what
  // lights up under the pointer, so the promise and the press cannot drift.
  const AxisRef* GrabbableAt(std::size_t index, Vector2 pointer, const Viewport& viewport,
                             FontStore& fonts) const;
  // The marker nearest the pointer on this row, whether or not it is one you
  // could take hold of. Pointing at a derived value still says which thing it
  // is -- it just cannot be moved.
  const AxisRef* MarkerNear(std::size_t index, Vector2 pointer, const Viewport& viewport,
                            FontStore& fonts) const;
  // Which row is offering to take a value, or kNoRow. The rim carries the row
  // and a marker is a value that already exists, so only the bare axis offers.
  static constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);
  std::size_t OfferedRow(Vector2 pointer, const Viewport& viewport, FontStore& fonts) const;
  bool        OverMarker(std::size_t index, Vector2 pointer, const Viewport& viewport,
                         FontStore& fonts) const;
  // The box a row writes into: the one it docks under, which stays put when
  // lines merge, so a shared axis still has one answer.
  CodeBlock*  Owner(const NumberLine& line) const;
  // Writes a value into that box at the place on the axis the pointer is, and
  // says what it called it -- empty if it could not. The caller carries on
  // holding the new marker, so making one and moving one are the same gesture.
  std::string MakeVariable(std::size_t index, Vector2 pointer, const Viewport& viewport,
                           FontStore& fonts);
  const mathprogram::Variable* Resolve(const AxisRef& ref) const;
  // Where a variable's marker is drawn on its row: the dot that *is* the
  // variable, which is where a wire from its name should land.
  Vector2 MarkerAt(std::size_t index, const AxisRef& ref, const Viewport& viewport,
                   FontStore& fonts) const;
  // Points the line at everything now on it. Used when two lines merge, since
  // the host's old window need not contain the arriving value at all.
  void Reframe(NumberLine& line) const;

  std::vector<NumberLine> lines_;

  // A value made by clicking an axis belongs on *that* axis, at the place it
  // was put -- not on whatever axis its box would otherwise have given it. The
  // claim is held from the click until the build that creates the variable
  // comes back, since a ref to something that does not exist yet would be swept
  // away by the next Sync. The line is remembered by something already on it,
  // so merging or throwing away another line cannot make it point elsewhere.
  struct Pending {
    AxisRef made;
    AxisRef anchor;
    bool    live = false;
  };
  Pending pending_;

  Drag        drag_ = Drag::None;
  std::size_t dragLine_ = 0;
  AxisRef     dragRef_;     // the handle being moved, for Drag::Handle
  Vector2     dragGrab_{};  // pointer offset within the row, for Drag::Move
  float       dragValueBefore_ = 0.0f;   // what a handle was on when it was taken
  // Where a row being carried is drawn. It keeps its slot in the stack until
  // it is dropped, so the row you are aiming at does not slide out from under
  // the pointer as you pick this one up.
  Vector2     dragScreenPos_{};
  bool        dragMoved_ = false;
};

}  // namespace sb
