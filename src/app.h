#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "types/RealViz.h"
#include "blocks.h"
#include "fontstore.h"
#include "types/RealFunctionViz.h"
#include "selection.h"
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
  // A click (press and release without dragging) selects a block; a drag pans
  // the canvas instead, so both gestures can start anywhere.
  void HandlePointer();
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

  // Where this canvas came from, so Ctrl+S can save straight back to it.
  std::string currentPath_;

  CodeBlock* focus_ = nullptr;
  CodeBlock* dragBlock_ = nullptr;
  Vector2    dragBlockGrab_{};   // world offset from the box origin to the pointer
  bool       dragBlockMoved_ = false;
  bool       pressActive_ = false;
  Vector2    pressOrigin_{};

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
    std::vector<std::pair<std::string, float>> values;
    Vector2                                    offset{};   // from the group's corner
  };
  std::vector<Clipping> clipboard_;
};

}  // namespace sb
