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
#include "ui.h"
#include "viewport.h"

namespace sb {

class App {
 public:
  bool Init(const std::string& assetDir, std::string* error);
  void Shutdown();

  // Opens a canvas file at startup, e.g. from the command line.
  bool OpenCanvas(const std::string& path);

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
  bool LoadCanvas(const std::string& path);

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
  // Pictures and video. Not mathematics, so not in types/ -- but windows, and
  // so subject to every rule the others are.
  MediaPanel                              media_;
  // Slides: regions of the canvas worth looking at on their own, and the button
  // that makes them.
  SlidePanel                              slides_;

  // Where this canvas came from, so Ctrl+S can save straight back to it.
  std::string currentPath_;

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
    bool                                       showMath = false;
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
