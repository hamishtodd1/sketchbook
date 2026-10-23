#pragma once

#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "types/RealViz.h"
#include "blocks.h"
#include "fontstore.h"
#include "types/RealFunctionViz.h"
#include "chat.h"
#include "conversation.h"
#include "media.h"
#include "selection.h"
#include "slides.h"
#include "colordw.h"
#include "shaderwindow.h"
#include "surfacedw.h"
#include "volumedw.h"
#include "spacedw.h"
#include "ui.h"
#include "viewport.h"
#include "viz.h"
#include "capture.h"
#include "recording.h"
#include "transition.h"

namespace sb {

// The size of type a new box opens at, in world units.
inline constexpr float kDefaultBlockEm = 26.0f;

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
  // Every code box on this canvas reads as code, never as mathematics.
  void SetAlwaysCode(bool on);
  // Dev: a conversation in the middle of the view, saying this. ` || ` splits
  // it into messages, each said once the reply before it is done; a message
  // `box:text` makes a box instead, and `point:x,y` puts the pointer there as
  // far as the conversation is concerned.
  void Converse(const std::string& text, const std::string& effort);
  // Dev: actions on the first conversation, comma-separated and done in order:
  // those ChatPanel::Dev knows, `discard` (thrown away, as an undo step) and
  // `undo`.
  void DevChat(const std::string& actions);
  // Dev: a box of these lines (`|` between them) with the caret in it, and the
  // keys (CodeBlock::DevKeys) done in it; its lines are logged afterwards. The
  // app adds `wait:N` (frames), `blur` (the caret out, as a click elsewhere)
  // and `focus` (back in).
  void DevKeys(const std::string& box, const std::string& keys);
  // Dev: the canvas as Claude is shown it.
  std::string Describe() { return DescribeCanvas(); }
  // Dev: writes the canvas to a file, as Ctrl+S to a new name would.
  bool SaveTo(const std::string& path) { return SaveCanvas(path); }

  // --- recording and playing back (recording.h) ------------------------------
  //
  // What a recording keeps of a frame besides the canvas (which it keeps as an
  // undo step has it): the camera, the pointer, the caret.
  recording::View    RecordedView();
  // **Recording** is started and stopped from the Escape menu. A recording goes
  // into a folder of its own under Videos/sketchbook, named for the canvas and
  // the time it began.
  //
  // **The camera first.** Opening the camera and the microphone takes Windows a
  // couple of seconds, and a session begun at once had no face or sound for
  // them. So pressing Record starts the capture and *arms*: the light is dim,
  // "starting..." beside it, and the session itself begins once ffmpeg says the
  // devices are delivering -- or at once if there are none, or after ten seconds
  // whatever. Pressing it again while armed stops, and leaves nothing behind.
  void StartRecording();
  void StopRecording();
  bool Recording() const { return recorder_.Recording() || arming_; }

  // **Passive**: the canvas takes no input and keeps no undo -- it is being
  // played, and the pointer and keys are the editor's.
  void SetPassive(bool on) { passive_ = on; }
  // Ready to play a recording of the canvas at `canvasPath`: passive, never
  // saving, and reading pictures relative to where that canvas was.
  void BeginPlayback(const std::string& canvasPath) {
    passive_ = true;
    autosaveOn_ = false;
    currentPath_ = canvasPath;
    untitled_ = false;
  }
  // Shows a recorded frame. The text of the box being typed in is put back as
  // typing left it, not built, so a recording shows no compile its typist did
  // not ask for; anything else is put back as an undo step is.
  // `jumped` is a frame not straight after the last one shown, whose builds in
  // between were skipped: everything is built then, but the box being typed in.
  // `lastBuilt` is, on a jump, what the box being typed in was last built from.
  void ShowRecorded(const std::vector<std::string>& canvas, const recording::View& view,
                    bool jumped, const std::vector<std::string>& lastBuilt = {});

  // --- a transition across a cut (app_cut.cpp) -------------------------------
  //
  // One end of a cut: the canvas and the view, as the recording has them.
  struct CutEnd {
    std::vector<std::string> canvas;
    recording::View          view;
  };
  // Shows the transition from `before` to `after`, `through` of the way across
  // (0 to 1, eased here). The canvas is the one after, built; on top of it the
  // camera eases from one view to the other, each box glides from where it was
  // to where it is, its text morphs if it changed, its knobs ease from their
  // numbers to the new ones, a box that went fades out and one that came fades
  // in. Both ends are measured the first time a `key` is seen.
  // `shiftBefore` and `shiftAfter` are the edit's moves of boxes at either end.
  struct BoxShift {
    int   index = -1;   // the box's place on the canvas at that end
    float dx = 0.0f, dy = 0.0f, scale = 1.0f;
  };
  void ShowCut(const std::string& key, const CutEnd& before, const CutEnd& after, double through,
               const std::vector<BoxShift>& shiftBefore = {},
               const std::vector<BoxShift>& shiftAfter = {});

  // **An edit moves boxes**: put each where the canvas has it, moved by so much
  // and its type scaled by so much -- the box, not the canvas, which is not
  // changed. Every frame, after the frame is shown; a box not mentioned is
  // where the canvas has it.
  void      ShiftBoxes(const std::vector<BoxShift>& shifts);
  // The box at a point of the screen (the editor's frame, while it draws one),
  // by its place on the canvas; -1 if none. And where it is, and which it is.
  int       BoxUnder(Vector2 point);
  // Where a point of the world is on the screen (or the editor's frame).
  Vector2   ToScreen(Vector2 world) const { return viewport_.ToScreen(world); }
  Rectangle BoxRect(int index);
  long long BoxId(int index) const;

  FontStore& Fonts() { return fonts_; }

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

  // --- where a new window goes ---------------------------------------------
  //
  // **A new window goes in the nearest gap to the middle of the view**, centred
  // there if it can be: every window already on the canvas stays where it is,
  // and the new one is put where it overlaps none of them, with room round it.
  // Then, if that is not the middle, the camera glides until it is.
  //
  // Every window on the canvas, in world units.
  std::vector<Rectangle> WindowRects();
  // The top-left of the nearest place to the middle of the view where a window
  // of this size fits among `taken`.
  Vector2 FindGap(Vector2 size, const std::vector<Rectangle>& taken);
  // Glides the camera to centre a rectangle of the canvas, unless it already is.
  void    CentreOn(Rectangle world);

  // --- Claude's tools on the canvas (app_tools.cpp) ------------------------
  //
  // A conversation's reply hands out the tools it wants; they are carried out
  // here, on the thread that owns the canvas. Some answer at once; making or
  // editing a box waits until it has compiled and its values are known, and a
  // look waits for the next frame to be drawn.
  struct ToolJob {
    ChatWindow*      window = nullptr;
    claude::ToolCall call;
    CodeBlock*       waitFor = nullptr;   // answered once this box has settled
    int              frames = 0;
    std::string      said;                // what to say before the box's report
    bool             look = false;
    Rectangle        lookWorld{};         // what a look frames
    std::string      lookKind;
  };
  std::vector<ToolJob> toolJobs_;
  std::vector<std::string> converseQueue_;   // dev: messages still to say
  CodeBlock*               devKeysBlock_ = nullptr;
  std::string              devKeysLeft_;
  int                      devKeysWait_ = 0;
  void                     DevKeysStep();
  std::string              converseEffort_;
  bool                     devPointed_ = false;
  Vector2                  devPointer_{};
  void                     ConverseNext();
  // Once a frame: hands out new calls, and answers the ones that have settled.
  void        ServeTools();
  // Carries out a call; false when it has to wait.
  bool        StartTool(ToolJob& job);
  // Whether a box a job is waiting on has compiled and been read.
  bool        Settled(const ToolJob& job) const;
  // At the start of drawing: a look waiting on this frame is drawn first,
  // framed on its box, and read back as a picture.
  void        TakeLooks();
  std::string DescribeCanvas();
  // One box as Claude is shown it. Nothing in it that changes without the box
  // changing -- where the caret is goes in the status instead -- so an unchanged
  // box reads the same and need not be sent again.
  std::string DescribeBox(CodeBlock& block);
  // The canvas as a message carries it: every box, and where Hamish is.
  claude::CanvasView ViewCanvas();
  // Where the canvas asks for a new value: the vizes' `example`s.
  std::function<bool(const clifford::Signature&, const std::string&, const std::string&,
                     std::vector<float>*)>
  ExampleSource();
  // The display window under a point on screen, in world units, and what kind.
  bool DwUnder(Vector2 point, Rectangle* world, std::string* kind);
  CodeBlock*  BoxNamed(const std::string& id);
  // A change Claude made is a change somebody made: its own undo step.
  void        BeforeToolChange();
  void        AfterToolChange();
  // Everything on the canvas, without the chrome over it. A look draws this a
  // second time, unseen, framed on what it is looking at.
  void        DrawCanvas(bool look);
  // Puts the canvas back as a snapshot says, as an undo step: what a rewind
  // that takes the canvas back does.
  void        RestoreCanvas(const std::string& snapshot);

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
  // Dragging a box's or a picture's grip: it scales, keeping its shape.
  bool HandleGrip();
  void StartGrip(CodeBlock* block, int item, Vector2 fromWorld);
  void StartDwGrip(Resizable* panel, int index, Vector2 fromWorld);
  void MoveGrip(Vector2 world);
  void EndGrip();
  struct Grip {
    CodeBlock*             block = nullptr;   // or
    int                    item = -1;         // a picture, or
    Resizable*             panel = nullptr;   // a display window,
    int                    index = -1;        // that one of its kind
    float                  startSize = 1.0f;
    Rectangle              start{};           // where it was, world
    Vector2                from{};            // where the pointer was, world
    float                  startEm = 0.0f;
    bool                   snaps = false;     // pictures and prose: see resize.h
    std::vector<Rectangle> neighbours;
  };
  Grip grip_;
  bool Gripping() const {
    return grip_.block != nullptr || grip_.item >= 0 || grip_.panel != nullptr;
  }
  // Every kind of display window, in the order they are drawn.
  std::vector<Resizable*> Resizables() {
    return {&axes_, &plots_, &shaders_, &colours_, &surfaces_, &volumes_, &spaces_};
  }

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
  // Gathers what every box shares, and rebuilds any box whose share of it has
  // changed. Once a frame, before the boxes compile.
  void ShareAcrossCanvas();
  // The box a name belongs to as seen from `block`: the block itself if it
  // defines the name, otherwise whichever box shares it -- or `block`, if none.
  CodeBlock* HomeOf(CodeBlock* block, const std::string& name) const;
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
  // `withView` is the file: the camera, and every open conversation in full.
  // Otherwise an undo step: no camera, and conversations only by reference --
  // unless `withChats` is false, which is the canvas a rewind puts back, and
  // which leaves conversations alone entirely.
  void WriteCanvas(std::ostream& out, const std::string& path, bool withView,
                   bool withChats = true);
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
  bool                                    passive_ = false;
  long long                               lastBoxId_ = 0;   // the last id a box was given
  // The transition being shown, if one is (app_cut.cpp).
  struct CutBox {
    CodeBlock*                                  block = nullptr;   // after the cut
    Vector2                                     from{}, to{};      // world
    bool                                        morphs = false;
    morph::Morph                                text;   // box-local world units
    std::vector<std::tuple<std::string, float, float>> values;
    float                                       emFrom = 0.0f, emTo = 0.0f;
    bool                                        arrives = false;
  };
  // A box that went, with its windows: its part of a picture of the canvas as
  // it was before the cut, fading where it was.
  struct CutGhost {
    Rectangle world{};
    Rectangle shot{};   // in the picture's pixels
  };
  // A window not docked to any box, gliding from where it was to where it is.
  struct CutWindow {
    Resizable* panel = nullptr;
    int        index = -1;
    Vector2    from{}, to{};
  };
  struct Cut {
    std::string           key;
    bool                  active = false;
    recording::View       before, after;
    std::vector<CutBox>    boxes;
    std::vector<CutGhost>  ghosts;
    std::vector<CutWindow> windows;
    // Pictures and videos, by their place in the list, from where to where.
    std::vector<std::tuple<std::size_t, Vector2, Vector2>> media;
    RenderTexture2D        shot{};   // the canvas before the cut, if a ghost wants it
    double                through = 0.0;
  };
  Cut                                     cut_;
  std::vector<bool>                       shifted_;   // boxes an edit has moved
  transition::OutlineFill                 cutFill_;
  void                                    LoadPlayed(const std::vector<std::string>& canvas);
  void                                    EndCut();
  void                                    DrawCut();
  recording::Recorder                     recorder_;
  capture::Capture                        capture_;
  bool                                    arming_ = false;
  double                                  armedAt_ = 0.0;
  std::string                             armedFolder_;
  void                                    BeginSession(const std::string& folder);
  void                                    TrackArming();
  double                                  recordStart_ = 0.0;
  double                                  recordCost_ = 0.0;   // seconds, in all
  int                                     recordFrames_ = 0;
  bool                                    replayHeld_ = false;
  // Playing: a box read back is not built until the recording says it was.
  bool                                    deferBuilds_ = false;
  // Recording: how many builds each box had had, to see which built this frame.
  std::vector<std::pair<const CodeBlock*, int>> builtSoFar_;
  std::vector<std::string>                shownCanvas_;   // the frame last played
  Options                                 options_;
  OptionsMenu                             menu_;
  std::vector<std::unique_ptr<CodeBlock>> codeBlocks_;
  std::vector<MathBlock>                  mathBlocks_;
  // The number lines and plots belong to the canvas, not to any one box, so a
  // set of axes can carry variables from two different boxes at once.
  AxisPanel                               axes_;
  PlotPanel                               plots_;
  ShaderPanel                             shaders_;
  ColorPanel                              colours_;
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
  // Conversations with Claude. On top of every other window.
  ChatPanel                               chats_;
  // What the boxes offer each other, and which version of each box it was
  // gathered from -- so it is gathered again only when something changed.
  mathprogram::Canvas                     canvas_;
  std::vector<std::pair<const mathprogram::Program*, unsigned long>> sharedFrom_;
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
  // The last press on a box's text, so a second one can be known as a double
  // click. Only compared, never followed, so a box deleted since is harmless.
  const CodeBlock* lastPressBlock_ = nullptr;
  double           lastPressTime_ = -1.0;
  Vector2          lastPressAt_{};
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
