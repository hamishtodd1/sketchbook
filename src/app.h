#pragma once

#include <memory>
#include <string>
#include <vector>

#include "types/RealViz.h"
#include "blocks.h"
#include "fontstore.h"
#include "types/RealFunctionViz.h"
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

  // Where this canvas came from, so Ctrl+S can save straight back to it.
  std::string currentPath_;

  CodeBlock* focus_ = nullptr;
  CodeBlock* dragBlock_ = nullptr;
  Vector2    dragBlockGrab_{};   // world offset from the box origin to the pointer
  bool       pressActive_ = false;
  Vector2    pressOrigin_{};
};

}  // namespace sb
