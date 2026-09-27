#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "types/RealViz.h"
#include "blocks.h"
#include "fontstore.h"
#include "types/RealFunctionViz.h"
#include "media.h"
#include "selection.h"
#include "slides.h"
#include "shaderwindow.h"
#include "surfacedw.h"
#include "volumedw.h"
#include "spacedw.h"
#include "ui.h"
#include "viewport.h"
#include "viz.h"

namespace sb {

class App {
 public:
  bool Init(const std::string& assetDir, std::string* error);
  void Shutdown();

  // Opens a canvas file at startup, e.g. from the command line.
  bool OpenCanvas(const std::string& path);
  // Auto-save is off for a capture -- a run that takes a screenshot and quits --
  // since whatever reaches its window by accident is not somebody's work to keep.
  void SetAutosave(bool on) { autosaveOn_ = on; }
  // Fills the monitor the window is on, without a title bar, or puts the window
  // back as it was.
  void SetFullscreen(bool on);

  void Update();
  void Draw();

 private:
  // Pressing inside a box gives it the caret and starts selecting its text; a
  // press on text already selected picks that text up instead, and pulling it
  // out of the box gives its variable a window of its own. Returns true while
  // it owns the pointer.
  bool HandleTextGesture();
  void HandleTyping();
  // Moving focus away is one of the two triggers for a rebuild.
  void SetFocus(CodeBlock* block);
  void RemoveBlock(CodeBlock* block);
  CodeBlock* BlockAt(Vector2 screenPoint);
  CodeBlock* BlockRimAt(Vector2 screenPoint);
  // Grabbing a window by its rim moves it. Returns true while the rim owns the
  // pointer, so nothing behind it pans or takes focus.
  bool       HandleBlockDrag();
  CodeBlock* NewBlockAtCentre();

  // Boxes live on the grid and never overlap. Every placement goes through
  // PlaceBlock, which snaps the top-left to the nearest dot and then steps the
  // box clear of anything already sitting there.
  Rectangle Footprint(const CodeBlock* block);
  void      PlaceBlock(CodeBlock* block, Vector2 desired);
  void      ResolveOverlaps(CodeBlock* block);
  // A box grows number lines the first time it compiles, so a footprint can
  // widen with no one having moved anything. Run when that happens.
  void      SettleBlocks();

  // The wheel over a window resizes it uniformly. It lives here rather than in
  // any one panel because the snapping rule has to see every kind of window at
  // once: a picture matching the height of the picture beside it, or a caption
  // matching the width of what it is under.
  bool HandleResize();

  // Files dropped on the window: a picture or a video becomes a window where
  // the pointer is, and a .sketch opens.
  void HandleDroppedFiles();

  // --- taking hold of two values at once -----------------------------------
  //
  // A finger on each of two vizes means "these two, together". While they are
  // held they are **frozen** -- a finger on a handle would otherwise drag it,
  // and what the two fingers mean is not what either means alone -- and a wire
  // follows each finger back to the value it came from. Bring the fingers
  // together and a little menu of operators appears where they met; touch one
  // and a line defining the two of them that way is written at the caret.
  //
  // Returns true while the gesture owns the input.
  bool HandleCompose();
  void DrawCompose();
  // Pulling a viz out of the window it is in. Ctrl and a left drag, or a finger
  // holding the window while another drags the viz; once it leaves the window
  // it is a window of its own, being carried. Returns true while it owns input.
  bool HandlePull();
  // Feeding every function that is being read, from whatever the canvas says a
  // spread of its input type looks like.
  void FeedFunctions();
  // The pair the pointer is on -- one sample and the answer it turned into --
  // boxed at both ends and joined by a wire.
  void DrawSampleLink();
  // Writes `float <fresh> = <a> <op> <b>;` into the box the two came from.
  void ComposeInto(const std::string& op);

  // A value defined out of other values is the end of a little machine, and
  // pointing at it draws the machine: wires out of the windows of the values it
  // is made from, through a box for each operator, to every place this one is
  // written and to its own windows. Returns true if it drew one, in which case
  // the box must not also draw its plain wire.
  bool DrawWiring(const ActiveValue& active);
  // Whether there is a machine worth drawing for this value: it is made out of
  // other values, and at least one of them is on screen somewhere.
  bool CanWire(const ActiveValue& active);
  // Where a named variable of a box is shown, across every kind of window.
  std::vector<Vector2> WindowsOf(const CodeBlock* block, const std::string& name);

  // A new slide's title and body, which are ordinary text boxes.
  void FurnishSlide(Vector2 centre, const std::string& title,
                    const std::vector<std::string>& body);
  // A slide thrown away takes what is on it with it, and the deck below closes
  // the gap. Which windows those are is decided exactly as the lasso decides:
  // by their middles.
  void ThrowAwaySlide(Rectangle bounds, float shift);
  // Selects every window whose middle is inside a world rectangle, by handing
  // the rectangle to the lasso's own machinery.
  void SelectWithinWorld(Rectangle worldRect);
  // Glides the camera so that every window whose middle is on screen is framed,
  // centred and filling the view. The Home key.
  void FrameOnScreen();

  // --- selection ----------------------------------------------------------
  //
  // Dragging the background draws a lasso; what it catches behaves as one
  // window from then on -- it moves together, it can be carried onto the drop
  // target together, and Delete throws it all away.
  void HandleSelection();
  void ClearSelection();
  bool AnySelected() const;
  bool SelectedRimAt(Vector2 point);
  void DeleteSelection();
  void CopySelection(bool cut);
  void PasteClipboard();

  bool SaveCanvas(const std::string& path);
  // A blank canvas, after the one being left has been written down. It has no
  // file until something is put on it, and then it gets an untitled one.
  void NewCanvas();
  bool LoadCanvas(const std::string& path);
  // The canvas as text, and back. Saving and loading are these plus a file; an
  // undo step is these plus a string. `withView` is false for a snapshot, since
  // where you are looking is not something an undo should move; `restoring` keeps
  // the camera, reuses every box whose text has not changed -- so undoing a move
  // does not rebuild every shader on the canvas -- and leaves positions exactly
  // as written rather than snapping them again at today's zoom.
  void WriteCanvas(std::ostream& out, const std::string& path, bool withView);
  bool ReadCanvas(std::istream& in, const std::string& path, bool restoring);

  // --- canvas undo ----------------------------------------------------------
  //
  // **An undo step is the whole canvas, as the text it would be saved as.** Not
  // a list of operations each with an inverse: there are dozens of things that
  // change a canvas -- carrying, merging, closing, pulling out, resizing, typing
  // a box, throwing a slide away -- and every one of them would need its inverse
  // written and kept right. The canvas file already says everything that
  // matters, so a step is a snapshot of it and undoing is loading it back.
  //
  // A snapshot is taken once things have settled after something you did, so a
  // drag is one step and a run of wheel clicks is one step, and a box being
  // written in is one step when the caret leaves it (inside a box, Ctrl+Z is
  // still the box's own). A change nobody made -- a compile finishing, an axis
  // framing itself -- is folded into the current state rather than becoming a
  // step you would have to undo past.
  // --- auto-save ------------------------------------------------------------
  //
  // **The canvas saves itself.** Whenever an undo step is taken -- which is to
  // say, whenever something you did has settled -- and every few seconds while a
  // box is being written in, the canvas is written back to the file it came
  // from. A canvas that never came from a file is given one, among the user's
  // settings, and it appears in the recent list: nothing made here is ever only
  // in memory. Ctrl+S on one of those still asks for a name, and the untitled
  // file goes once there is a real one.
  //
  // Opening a canvas is not a change, so merely looking at one never rewrites
  // it. And a save is written beside the file and moved over it, so a crash in
  // the middle of one cannot leave half a canvas behind.
  void ScheduleAutosave() { autosaveDue_ = GetTime() + 0.5; }
  void TrackAutosave(bool held);
  void FlushAutosave();
  bool WriteCanvasFile(const std::string& path);

  void ResetUndo();
  void TrackUndo(bool held);
  void CommitUndo();
  void StepUndo(bool redo);
  std::string Snapshot();

  FontStore                               fonts_;
  Viewport                                viewport_;
  Options                                 options_;
  OptionsMenu                             menu_;
  std::vector<std::unique_ptr<CodeBlock>> codeBlocks_;
  std::vector<MathBlock>                  mathBlocks_;
  // The number lines and plots belong to the canvas, not to any one box, so a
  // set of axes can carry variables from two different boxes at once.
  AxisPanel                               axes_;
  PlotPanel                               plots_;
  ShaderPanel                             shaders_;
  // One kind of window per shape of function: a colour-valued map of the plane
  // is a picture with no camera, a real-valued one is a surface under an
  // orthographic camera, and a function of three variables is a volume under a
  // perspective one.
  SurfacePanel                            surfaces_;
  VolumePanel                             volumes_;
  SpacePanel                              spaces_;
  // Pictures and video. Not mathematics, so not in types/ -- but windows, and
  // so subject to every rule the others are.
  MediaPanel                              media_;
  // Slides: regions of the canvas worth looking at on their own, and the button
  // that makes them.
  SlidePanel                              slides_;
  // How each type is drawn, as written on this very canvas. A type with no viz
  // boxes keeps the drawing the app was born with, so an ordinary canvas needs
  // to know nothing about any of this.
  VizStore                                vizes_;

  // Where this canvas came from, so Ctrl+S can save straight back to it.
  std::string currentPath_;

  std::vector<std::string> undo_;
  std::vector<std::string> redo_;
  std::string              undoBase_;          // the canvas as of the last step
  bool                     undoPending_ = false;   // something was done since
  double                   undoLastInput_ = 0.0;
  double                   undoNextCheck_ = 0.0;

  std::string              savedSnapshot_;     // the canvas as it was last written
  double                   autosaveDue_ = 0.0; // 0 when nothing is waiting
  double                   typingSavedAt_ = 0.0;
  bool                     untitled_ = false;  // currentPath_ is one we made up
  bool                     autosaveOn_ = true;
  bool                     wasPlaying_ = false;   // so a swing stopping can be saved

  CodeBlock* focus_ = nullptr;
  CodeBlock* dragBlock_ = nullptr;
  Vector2    dragBlockGrab_{};   // world offset from the box origin to the pointer
  bool       dragBlockMoved_ = false;
  // Selecting text in a box, and pulling a variable out of it. `Arming` is the
  // moment between pressing on a selection and having moved far enough for it
  // to be a drag rather than a click.
  enum class TextDrag { None, Selecting, Arming };
  TextDrag    textDrag_ = TextDrag::None;
  CodeBlock*  textBlock_ = nullptr;
  std::string pullName_;
  Vector2     pullFrom_{};
  // True while the system cursor is hidden because a hand is being drawn in its
  // place. Kept so the two can never get out of step.
  bool       cursorHidden_ = false;

  // The grid is a placement aid, not scenery, so it is only up while something
  // is being placed. `gridUntil_` is when the linger after the last such
  // gesture runs out; `gridShow_` chases it, so the dots fade rather than blink.
  float      gridShow_ = 0.0f;
  double     gridUntil_ = 0.0;

  // What is in use this frame, decided while updating and drawn afterwards.
  std::vector<ActiveValue> active_;

  // Where the window was, and how big, before it filled the screen.
  Vector2 windowedAt_{};
  int     windowedWidth_ = 0;
  int     windowedHeight_ = 0;

  // A viz being pulled out of its window, until it leaves it.
  enum class PullFrom { None, Axis, Space, Plot };
  struct Pull {
    PullFrom    from = PullFrom::None;
    CodeBlock*  block = nullptr;
    std::string name;
    bool        field = false;    // a function's field on an axis, not a value
    Rectangle   window{};         // the window it has to leave
    // A plot with several curves on it cannot say which is under the pointer
    // until it has drawn them, so the pull waits for that answer.
    bool        picking = false;
    // Pulled by a finger rather than the mouse, and which one.
    bool         byFinger = false;
    unsigned int finger = 0;
  };
  Pull pull_;
  // The viz under a point, as a pull that has not started yet. Shared by the
  // mouse and the finger, so the two cannot disagree about what is pullable.
  bool FindPullable(Vector2 at, Pull* out);
  // A window pulled out by a finger is carried by that finger (carry.h) until
  // it lifts.
  bool         fingerCarry_ = false;
  unsigned int fingerCarryId_ = 0;
  // Which fingers were down last frame, so a finger that has just landed can be
  // told from one that was already holding something.
  unsigned int touchSeen_[touch::kMaxPoints]{};
  int          touchSeenCount_ = 0;

  // The two-finger gesture that composes two values into a third.
  enum class Compose { None, Holding, Menu };
  Compose     compose_ = Compose::None;
  CodeBlock*  composeBlock_ = nullptr;
  std::string composeNames_[2];
  Vector2     composeAt_[2]{};      // where each finger is
  Vector2     composeMenu_{};       // where the two met
  Vector2     composeTip_{};        // where the finger choosing from it is
  float       composeApart_ = 0.0f; // how far apart they began
  int         composeHot_ = -1;     // which option that finger is over
  unsigned int composeIds_[2]{};

  // The loop being drawn, in screen points, while the background is dragged.
  std::vector<Vector2> lasso_;
  bool                 lassoActive_ = false;

  // Moving everything selected at once. The whole group translates rigidly by a
  // whole number of grid steps, so their arrangement survives the move exactly.
  bool                                             selectionDrag_ = false;
  bool                                             selectionMoved_ = false;
  Vector2                                          selectionGrab_{};
  std::vector<std::pair<CodeBlock*, Vector2>>      selectionFrom_;

  // What Ctrl+C took. Only boxes: a number line without the box whose variable
  // it stands for would be a window about nothing.
  struct Clipping {
    std::vector<std::string>                   lines;
    float                                      em = 26.0f;
    // How the box reads: 0 auto, 1 mathematics, 2 code.
    int                                        showMath = 0;
    // How a text box is set. A copy that lost its formatting would not be one.
    bool                                       bold = false;
    bool                                       italic = false;
    int                                        justify = 0;
    float                                      textWidth = 0.0f;
    bool                                       showSource = false;
    std::vector<std::pair<std::string, float>> values;
    Vector2                                    offset{};   // from the group's corner
  };
  std::vector<Clipping> clipboard_;
};

}  // namespace sb
