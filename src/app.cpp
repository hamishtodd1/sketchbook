#include "app.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

#include "compiler.h"
#include "filedialog.h"
#include "grid.h"
#include "text.h"
#include "theme.h"

namespace sb {
namespace {

// A press that moves less than this is a click, not a drag.
constexpr float kClickSlop = 6.0f;

constexpr float kDefaultBlockEm = 26.0f;

// Bumped whenever the on-disk shape changes.
constexpr int kFileVersion = 2;

}  // namespace

bool App::Init(const std::string& assetDir, std::string* error) {
  if (!fonts_.Load(assetDir + "/fonts", error)) return false;

  viewport_.Reset({0.0f, 0.0f}, 1.0f);
  // Plots stack under the number lines, so they have to know how tall those are.
  plots_.SetAxes(&axes_);

  if (compiler::ToolchainPath().empty()) {
    TraceLog(LOG_WARNING,
             "COMPILER: no C++ compiler found; set SKETCHBOOK_CXX to enable code blocks");
  } else {
    TraceLog(LOG_INFO, "COMPILER: using %s", compiler::ToolchainPath().c_str());
  }

  // The starting card: the smallest thing that shows the whole loop. Two
  // chosen reals and one derived from them.
  // Its corner is a multiple of the base grid step, like every other box.
  codeBlocks_.push_back(std::make_unique<CodeBlock>(
      Vector2{-320.0f, -200.0f}, kDefaultBlockEm,
      std::vector<std::string>{"float a;", "float b;", "float c = (a + b) / 2;"}));
  codeBlocks_[0]->RequestCompile();

  return true;
}

void App::Shutdown() { fonts_.Unload(); }

bool App::OpenCanvas(const std::string& path) { return LoadCanvas(path); }

CodeBlock* App::BlockAt(Vector2 screenPoint) {
  // Topmost first, matching the draw order.
  for (auto it = codeBlocks_.rbegin(); it != codeBlocks_.rend(); ++it) {
    if ((*it)->Contains(screenPoint, viewport_, fonts_)) return it->get();
  }
  return nullptr;
}

CodeBlock* App::BlockRimAt(Vector2 screenPoint) {
  for (auto it = codeBlocks_.rbegin(); it != codeBlocks_.rend(); ++it) {
    if ((*it)->OnRim(screenPoint, viewport_, fonts_)) return it->get();
  }
  return nullptr;
}

// ------------------------------------------------------------------ placement
//
// Two rules hold for every box on the canvas, however it got there: its
// top-left sits on a grid dot, and it does not overlap another box. Both are
// enforced in one place, so there is no way to put a box down that breaks them.

Rectangle App::Footprint(const CodeBlock* block) {
  Rectangle rect = block->Measure(viewport_, fonts_).rect;

  // The number lines docked under a box move with it and read as part of it,
  // so a box that clears the card but lands on the knobs has not cleared.
  const Rectangle docks[] = {axes_.DockedBounds(block, viewport_, fonts_),
                             plots_.DockedBounds(block, viewport_, fonts_)};
  for (const Rectangle& dock : docks) {
    if (dock.width <= 0.0f || dock.height <= 0.0f) continue;
    const float right = std::max(rect.x + rect.width, dock.x + dock.width);
    const float bottom = std::max(rect.y + rect.height, dock.y + dock.height);
    rect.x = std::min(rect.x, dock.x);
    rect.y = std::min(rect.y, dock.y);
    rect.width = right - rect.x;
    rect.height = bottom - rect.y;
  }

  const float   zoom = std::max(viewport_.Zoom(), 1e-6f);
  const Vector2 topLeft = viewport_.ToWorld({rect.x, rect.y});
  return {topLeft.x, topLeft.y, rect.width / zoom, rect.height / zoom};
}

void App::PlaceBlock(CodeBlock* block, Vector2 desired) {
  if (block == nullptr) return;
  block->SetWorldPosition(sb::SnapToGrid(desired, viewport_.Zoom()));
  ResolveOverlaps(block);
}

void App::SettleBlocks() {
  // Earlier boxes hold their ground, so the one that just grew stays where the
  // user last put it and its neighbours are the ones that move.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) ResolveOverlaps(block.get());
}

void App::ResolveOverlaps(CodeBlock* block) {
  if (block == nullptr) return;

  const float step = GridStep(viewport_.Zoom());
  // Rims are drawn outside their box, so boxes that merely touch would have
  // fused rims. Keep both of them clear.
  const float margin = 2.0f * kRimThickness / std::max(viewport_.Zoom(), 1e-6f);

  // Only `block` moves, so everything it might land on is measured once. This
  // runs every frame of a drag, and Measure is not cheap.
  std::vector<Rectangle> others;
  others.reserve(codeBlocks_.size());
  for (const std::unique_ptr<CodeBlock>& other : codeBlocks_) {
    if (other.get() != block) others.push_back(Footprint(other.get()));
  }

  // Leave along whichever edge is nearest, a whole dot at a time so the box
  // stays on the lattice. A handful of passes settles any ordinary arrangement;
  // if one does not, leaving the box where it got to beats hunting the canvas
  // for a hole every frame of a drag.
  for (int pass = 0; pass < 8; ++pass) {
    const Rectangle mine = Footprint(block);
    const Rectangle grown = {mine.x - margin, mine.y - margin, mine.width + 2.0f * margin,
                             mine.height + 2.0f * margin};

    Rectangle theirs{};
    bool      blocked = false;
    for (const Rectangle& candidate : others) {
      if (!CheckCollisionRecs(grown, candidate)) continue;
      theirs = candidate;
      blocked = true;
      break;
    }
    if (!blocked) return;

    const float left = theirs.x - (grown.x + grown.width);
    const float right = (theirs.x + theirs.width) - grown.x;
    const float up = theirs.y - (grown.y + grown.height);
    const float down = (theirs.y + theirs.height) - grown.y;
    const float dx = (std::abs(left) < std::abs(right)) ? left : right;
    const float dy = (std::abs(up) < std::abs(down)) ? up : down;

    const auto wholeDots = [step](float distance) {
      return std::copysign(std::ceil(std::abs(distance) / step) * step, distance);
    };
    const Vector2 origin = block->WorldPosition();
    if (std::abs(dx) <= std::abs(dy)) {
      block->SetWorldPosition({origin.x + wholeDots(dx), origin.y});
    } else {
      block->SetWorldPosition({origin.x, origin.y + wholeDots(dy)});
    }
  }
}

bool App::HandleBlockDrag() {
  const Vector2 pointer = GetMousePosition();

  if (dragBlock_ != nullptr) {
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
      const Vector2 world = viewport_.ToWorld(pointer);
      PlaceBlock(dragBlock_, {world.x - dragBlockGrab_.x, world.y - dragBlockGrab_.y});
    } else {
      dragBlock_->SetDragging(false);
      dragBlock_ = nullptr;
    }
    return true;
  }

  CodeBlock* hit = BlockRimAt(pointer);
  if (hit == nullptr) return false;

  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    const Vector2 world = viewport_.ToWorld(pointer);
    const Vector2 origin = hit->WorldPosition();
    dragBlockGrab_ = {world.x - origin.x, world.y - origin.y};
    dragBlock_ = hit;
    hit->SetDragging(true);
  }
  // Hovering the rim is enough to claim the pointer: a click there is a grab,
  // never a pan or a caret placement.
  return true;
}

void App::RemoveBlock(CodeBlock* block) {
  if (block == nullptr) return;
  if (focus_ == block) focus_ = nullptr;
  if (dragBlock_ == block) dragBlock_ = nullptr;
  axes_.Forget(block);
  plots_.Forget(block);
  codeBlocks_.erase(std::remove_if(codeBlocks_.begin(), codeBlocks_.end(),
                                   [block](const std::unique_ptr<CodeBlock>& candidate) {
                                     return candidate.get() == block;
                                   }),
                    codeBlocks_.end());
}

void App::SetFocus(CodeBlock* block) {
  if (focus_ == block) return;

  if (focus_ != nullptr) {
    CodeBlock* previous = focus_;
    previous->SetFocused(false);
    focus_ = nullptr;

    // A block the user opened and left completely empty is not content. Drop it
    // before considering a build: destroying a block with a build in flight
    // would block on the future and stall the frame.
    if (previous->Empty() && previous->Status() == compiler::Status::NotRun &&
        previous->Variables().empty()) {
      if (block == previous) block = nullptr;
      RemoveBlock(previous);
    } else if (previous->Dirty()) {
      // Deselecting is a signal to rebuild -- but only if the text actually
      // changed, so clicking around does not kick off pointless builds.
      previous->RequestCompile();
    }
  }

  focus_ = block;
  if (focus_ != nullptr) focus_->SetFocused(true);
}

CodeBlock* App::NewBlockAtCentre() {
  const Vector2 centre = {static_cast<float>(GetScreenWidth()) * 0.5f,
                          static_cast<float>(GetScreenHeight()) * 0.5f};
  // Nudge up and left so the box grows around the middle of the screen rather
  // than hanging off to one side of it.
  const Vector2 world = viewport_.ToWorld({centre.x - 120.0f, centre.y - 30.0f});
  codeBlocks_.push_back(
      std::make_unique<CodeBlock>(world, kDefaultBlockEm, std::vector<std::string>{}));
  CodeBlock* block = codeBlocks_.back().get();
  // The middle of the screen may well be occupied; the new box goes to the
  // nearest free dot instead of opening on top of what is there.
  PlaceBlock(block, world);
  return block;
}

void App::HandlePointer() {
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    pressActive_ = true;
    pressOrigin_ = GetMousePosition();
  }

  if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT) || !pressActive_) return;
  pressActive_ = false;

  const Vector2 release = GetMousePosition();
  const float moved = std::hypot(release.x - pressOrigin_.x, release.y - pressOrigin_.y);
  if (moved > kClickSlop) return;  // that was a pan, not a click

  CodeBlock* hit = BlockAt(release);
  SetFocus(hit);
  if (hit != nullptr) hit->PlaceCaret(release, viewport_, fonts_);
}

void App::HandleTyping() {
  if (focus_ == nullptr) {
    // Typing with nothing selected starts a new box in the middle of the view.
    const int first = GetCharPressed();
    if (first <= 0) return;
    CodeBlock* block = NewBlockAtCentre();
    SetFocus(block);
    if (first >= 32) block->InsertCodepoint(first);
    block->HandleInput();  // whatever else arrived in the same frame
    return;
  }

  if (IsKeyPressed(KEY_ENTER) &&
      (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
    focus_->RequestCompile();
  }
  focus_->HandleInput();
}

void App::Update() {
  // Ctrl+S saves straight back to wherever this canvas came from, and only
  // asks when there is nowhere to go. Ctrl+O always asks.
  const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
  if (ctrl && IsKeyPressed(KEY_S)) {
    std::string path = currentPath_;
    if (path.empty()) path = filedialog::Save("canvas.sketch");
    if (!path.empty() && !SaveCanvas(path)) {
      TraceLog(LOG_WARNING, "CANVAS: could not save %s", path.c_str());
    }
  }
  if (ctrl && IsKeyPressed(KEY_O)) {
    const std::string path = filedialog::Open();
    if (!path.empty() && !LoadCanvas(path)) {
      TraceLog(LOG_WARNING, "CANVAS: could not load %s", path.c_str());
    }
  }

  if (IsKeyPressed(KEY_ESCAPE)) {
    menu_.Toggle();
    // Opening the menu ends editing, so no caret is left blinking behind it.
    if (menu_.Visible()) SetFocus(nullptr);
  }

  switch (menu_.Update(&options_)) {
    case MenuAction::Save: {
      const std::string path = filedialog::Save("canvas.sketch");
      if (!path.empty() && !SaveCanvas(path)) {
        TraceLog(LOG_WARNING, "CANVAS: could not save %s", path.c_str());
      }
      break;
    }
    case MenuAction::Load: {
      const std::string path = filedialog::Open();
      if (!path.empty()) {
        if (LoadCanvas(path)) menu_.Close();
        else TraceLog(LOG_WARNING, "CANVAS: could not load %s", path.c_str());
      }
      break;
    }
    case MenuAction::None:
      break;
  }

  const bool canvasActive = !menu_.Visible();

  // Whatever is already being dragged owns the pointer until the button comes
  // up. Without this a window let go over another window's rim would never see
  // the release, and would stay stuck to the cursor.
  bool consumed = false;
  if (canvasActive && dragBlock_ != nullptr) consumed = HandleBlockDrag();
  if (canvasActive && !consumed && axes_.Dragging()) {
    consumed = axes_.HandleInput(viewport_, fonts_, codeBlocks_);
  }
  if (canvasActive && !consumed && plots_.Dragging()) {
    consumed = plots_.HandleInput(viewport_, fonts_);
  }

  // Otherwise input goes to the innermost thing first: a focused box's buttons,
  // then the windows, then the canvas.
  if (canvasActive && !consumed && focus_ != nullptr) {
    consumed = focus_->HandleChrome(viewport_, fonts_);
  }
  if (canvasActive && !consumed) consumed = HandleBlockDrag();
  if (canvasActive && !consumed) consumed = axes_.HandleInput(viewport_, fonts_, codeBlocks_);
  if (canvasActive && !consumed) consumed = plots_.HandleInput(viewport_, fonts_);

  if (canvasActive && !consumed) {
    HandlePointer();
    HandleTyping();
  } else {
    pressActive_ = false;
  }

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->Poll();

  // A box asked to be closed by its own cross.
  for (std::size_t i = 0; i < codeBlocks_.size(); ++i) {
    if (!codeBlocks_[i]->CloseRequested()) continue;
    RemoveBlock(codeBlocks_[i].get());
    break;
  }

  // A box that has just gained (or lost) number lines is a different size than
  // it was a frame ago, so this is the moment to check nobody is sitting on
  // anybody. Only on the change: it is far too much work to do every frame.
  bool shapesChanged = axes_.Sync(codeBlocks_);
  shapesChanged = plots_.Sync(codeBlocks_) || shapesChanged;
  if (shapesChanged) SettleBlocks();

  // A window being hovered or dragged lights up the variable it stands for,
  // wherever that variable is written. Recomputed every frame: it is a property
  // of where the pointer is, not something to remember.
  std::vector<std::pair<CodeBlock*, std::string>> active;
  if (canvasActive) {
    axes_.ActiveVariables(viewport_, fonts_, &active);
    plots_.ActiveVariables(viewport_, fonts_, &active);
  }
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    std::vector<std::string> names;
    for (const auto& [owner, name] : active) {
      if (owner == block.get()) names.push_back(name);
    }
    block->SetHighlights(std::move(names));
  }

  // The canvas pans on the right button wherever the pointer is; the wheel and
  // touch belong to whatever window is under it.
  viewport_.Update(canvasActive, !consumed);
}

void App::Draw() {
  ClearBackground(kCanvasBg);

  if (options_.showGridDots) DrawDotGrid(viewport_);

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->Draw(viewport_, fonts_);
  for (const MathBlock& block : mathBlocks_) block.Draw(viewport_, fonts_);

  axes_.Draw(viewport_, fonts_);
  plots_.Draw(viewport_, fonts_);

  // Error balloons last, so they are never buried under a neighbouring card.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    block->DrawOverlay(viewport_, fonts_);
  }

  menu_.Draw(fonts_, options_);
}

// ---------------------------------------------------------------- persistence
//
// A plain line-based text format: readable, diffable, and trivial to extend.
// Each record starts with a keyword, and a block's source lines are prefixed
// with "> " so blank lines and leading spaces survive the round trip.

bool App::SaveCanvas(const std::string& path) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;

  file << "sketchbook " << kFileVersion << "\n";
  const Vector2 centre = viewport_.Centre();
  file << "view " << centre.x << " " << centre.y << " " << viewport_.Zoom() << "\n";
  file << "grid " << (options_.showGridDots ? 1 : 0) << "\n";

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    const Vector2 position = block->WorldPosition();
    file << "block " << position.x << " " << position.y << " " << block->Em() << " "
         << (block->ShowMath() ? 1 : 0) << "\n";
    for (const std::string& line : block->Lines()) file << "> " << line << "\n";
    for (const mathprogram::Variable& variable : block->Variables()) {
      // Only a real has a value worth keeping. A free function's choice lives in
      // its example generator, which is deterministic in the variable's name.
      if (variable.free && variable.kind == mathprogram::Kind::Real) {
        file << "val " << variable.name << " " << variable.value << "\n";
      }
    }
    file << "end\n";
  }

  auto indexOf = [this](const CodeBlock* block) {
    for (std::size_t i = 0; i < codeBlocks_.size(); ++i) {
      if (codeBlocks_[i].get() == block) return static_cast<int>(i);
    }
    return -1;
  };

  for (const NumberLine& line : axes_.Lines()) {
    file << "axis " << (line.docked ? 1 : 0) << " " << line.worldPos.x << " " << line.worldPos.y
         << " " << line.centre << " " << line.span << " " << indexOf(line.dock) << "\n";
    for (const AxisRef& ref : line.refs) {
      file << "ref " << indexOf(ref.block) << " " << ref.name << "\n";
    }
  }

  for (const Plot& plot : plots_.Plots()) {
    file << "plot " << (plot.docked ? 1 : 0) << " " << plot.worldPos.x << " " << plot.worldPos.y
         << " " << plot.centre.x << " " << plot.centre.y << " " << plot.span << " "
         << indexOf(plot.dock) << "\n";
    for (const CurveRef& ref : plot.refs) {
      file << "curve " << indexOf(ref.block) << " " << ref.name << "\n";
    }
  }
  if (!file.good()) return false;
  currentPath_ = path;
  return true;
}

bool App::LoadCanvas(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;

  std::string header;
  if (!std::getline(file, header)) return false;
  // Editors on Windows love to prepend a UTF-8 BOM; it is not our business to
  // reject a file over three invisible bytes.
  if (header.rfind("\xEF\xBB\xBF", 0) == 0) header.erase(0, 3);
  if (header.rfind("sketchbook", 0) != 0) return false;

  // Nothing is touched until the file has been read, so a malformed one cannot
  // leave the canvas half-replaced.
  struct LoadedBlock {
    Vector2                                   position{};
    float                                     em = kDefaultBlockEm;
    bool                                      showMath = false;
    std::vector<std::string>                  lines;
    std::vector<std::pair<std::string, float>> values;
  };
  struct LoadedAxis {
    bool                                     docked = true;
    Vector2                                  worldPos{};
    float                                    centre = 0.0f;
    float                                    span = 4.0f;
    int                                      dock = -1;
    std::vector<std::pair<int, std::string>> refs;
  };
  struct LoadedPlot {
    bool                                     docked = true;
    Vector2                                  worldPos{};
    Vector2                                  centre{};
    float                                    span = 6.0f;
    int                                      dock = -1;
    std::vector<std::pair<int, std::string>> refs;
  };

  std::vector<LoadedBlock> blocks;
  std::vector<LoadedAxis>  axes;
  std::vector<LoadedPlot>  plots;
  Vector2 centre{0.0f, 0.0f};
  float   zoom = 1.0f;
  bool    grid = true;
  bool    inBlock = false;

  std::string raw;
  while (std::getline(file, raw)) {
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    if (raw.empty()) continue;

    // Source lines are taken verbatim, so they must be tested before anything
    // is tokenised.
    if (inBlock && raw.rfind("> ", 0) == 0) {
      blocks.back().lines.push_back(raw.substr(2));
      continue;
    }
    if (inBlock && raw == ">") {
      blocks.back().lines.emplace_back();
      continue;
    }

    std::istringstream stream(raw);
    std::string keyword;
    stream >> keyword;

    if (keyword == "view") {
      stream >> centre.x >> centre.y >> zoom;
    } else if (keyword == "grid") {
      int value = 1;
      stream >> value;
      grid = (value != 0);
    } else if (keyword == "block") {
      LoadedBlock block;
      int showMath = 0;
      stream >> block.position.x >> block.position.y >> block.em >> showMath;
      block.showMath = (showMath != 0);
      blocks.push_back(std::move(block));
      inBlock = true;
    } else if (keyword == "val" && inBlock) {
      std::string name;
      float value = 0.0f;
      stream >> name >> value;
      blocks.back().values.emplace_back(name, value);
    } else if (keyword == "end") {
      inBlock = false;
    } else if (keyword == "axis") {
      LoadedAxis axis;
      int docked = 1;
      stream >> docked >> axis.worldPos.x >> axis.worldPos.y >> axis.centre >> axis.span >>
          axis.dock;
      axis.docked = (docked != 0);
      axes.push_back(std::move(axis));
    } else if (keyword == "ref" && !axes.empty()) {
      int index = -1;
      std::string name;
      stream >> index >> name;
      axes.back().refs.emplace_back(index, name);
    } else if (keyword == "plot") {
      LoadedPlot plot;
      int docked = 1;
      stream >> docked >> plot.worldPos.x >> plot.worldPos.y >> plot.centre.x >> plot.centre.y >>
          plot.span >> plot.dock;
      plot.docked = (docked != 0);
      plots.push_back(std::move(plot));
    } else if (keyword == "curve" && !plots.empty()) {
      int index = -1;
      std::string name;
      stream >> index >> name;
      plots.back().refs.emplace_back(index, name);
    }
  }

  // Commit.
  focus_ = nullptr;
  axes_.Clear();
  plots_.Clear();
  codeBlocks_.clear();
  mathBlocks_.clear();

  for (LoadedBlock& loaded : blocks) {
    auto block = std::make_unique<CodeBlock>(loaded.position, loaded.em, std::move(loaded.lines));
    block->SetShowMath(loaded.showMath);
    block->RestoreValues(std::move(loaded.values));
    block->RequestCompile();
    codeBlocks_.push_back(std::move(block));
  }

  std::vector<NumberLine> lines;
  for (const LoadedAxis& loaded : axes) {
    NumberLine line;
    line.docked = loaded.docked;
    line.worldPos = loaded.worldPos;
    line.centre = loaded.centre;
    line.span = loaded.span;
    if (loaded.dock >= 0 && loaded.dock < static_cast<int>(codeBlocks_.size())) {
      line.dock = codeBlocks_[static_cast<std::size_t>(loaded.dock)].get();
    }
    for (const auto& [index, name] : loaded.refs) {
      if (index < 0 || index >= static_cast<int>(codeBlocks_.size())) continue;
      line.refs.push_back({codeBlocks_[static_cast<std::size_t>(index)].get(), name});
    }
    if (!line.refs.empty()) lines.push_back(std::move(line));
  }
  axes_.Adopt(std::move(lines));

  std::vector<Plot> curves;
  for (const LoadedPlot& loaded : plots) {
    Plot plot;
    plot.docked = loaded.docked;
    plot.worldPos = loaded.worldPos;
    plot.centre = loaded.centre;
    plot.span = loaded.span;
    if (loaded.dock >= 0 && loaded.dock < static_cast<int>(codeBlocks_.size())) {
      plot.dock = codeBlocks_[static_cast<std::size_t>(loaded.dock)].get();
    }
    for (const auto& [index, name] : loaded.refs) {
      if (index < 0 || index >= static_cast<int>(codeBlocks_.size())) continue;
      plot.refs.push_back({codeBlocks_[static_cast<std::size_t>(index)].get(), name});
    }
    if (!plot.refs.empty()) curves.push_back(std::move(plot));
  }
  plots_.Adopt(std::move(curves));

  options_.showGridDots = grid;
  viewport_.Reset(centre, zoom);

  // Hold a loaded canvas to the same two rules as a live one. A file saved
  // under them reloads exactly as it was; an older one is tidied on the way in.
  // The zoom has to be restored first, since the lattice depends on it.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    PlaceBlock(block.get(), block->WorldPosition());
  }

  currentPath_ = path;
  return true;
}

}  // namespace sb
