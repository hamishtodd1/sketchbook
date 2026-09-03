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
  void Draw(const Viewport& viewport, FontStore& fonts) const;

  // The variables whose row is being hovered or dragged. Everything standing
  // for a variable wears its colour, and this is what says which one is in use
  // so its name can be boxed where it is written.
  void ActiveVariables(const Viewport& viewport, FontStore& fonts,
                       std::vector<ActiveValue>* out) const;

  // The screen rectangle the rows docked under a box cover, or a zero-size one
  // if nothing is docked there. They travel with the box, so as far as the
  // canvas is concerned they are part of it.
  Rectangle DockedBounds(const CodeBlock* block, const Viewport& viewport,
                         FontStore& fonts) const;

  // Takes a variable off the axis it shares with the rest of its box and starts
  // carrying it as an axis of its own, from the pointer. The drop is the same
  // drop any other carried row gets -- onto the canvas, onto another axis to
  // merge, or onto the cross.
  bool BeginPullOut(CodeBlock* block, const std::string& name, Vector2 pointer,
                    const Viewport& viewport);

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
  const mathprogram::Variable* Resolve(const AxisRef& ref) const;
  // Where a variable's marker is drawn on its row: the dot that *is* the
  // variable, which is where a wire from its name should land.
  Vector2 MarkerAt(std::size_t index, const AxisRef& ref, const Viewport& viewport,
                   FontStore& fonts) const;
  // Points the line at everything now on it. Used when two lines merge, since
  // the host's old window need not contain the arriving value at all.
  void Reframe(NumberLine& line) const;

  std::vector<NumberLine> lines_;

  Drag        drag_ = Drag::None;
  std::size_t dragLine_ = 0;
  AxisRef     dragRef_;     // the handle being moved, for Drag::Handle
  Vector2     dragGrab_{};  // pointer offset within the row, for Drag::Move
  // Where a row being carried is drawn. It keeps its slot in the stack until
  // it is dropped, so the row you are aiming at does not slide out from under
  // the pointer as you pick this one up.
  Vector2     dragScreenPos_{};
  bool        dragMoved_ = false;
};

}  // namespace sb
