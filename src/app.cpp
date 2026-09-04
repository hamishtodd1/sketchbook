#include "app.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

#include "compiler.h"
#include "filedialog.h"
#include "grid.h"
#include "recent.h"
#include "resize.h"
#include "text.h"
#include "theme.h"
#include "touch.h"

namespace sb {
namespace {

// A press that moves less than this is a click, not a drag.
constexpr float kClickSlop = 6.0f;

constexpr float kDefaultBlockEm = 26.0f;

// Bumped whenever the on-disk shape changes.
constexpr int kFileVersion = 3;

}  // namespace

bool App::Init(const std::string& assetDir, std::string* error) {
  if (!fonts_.Load(assetDir + "/fonts", error)) return false;

  // One dot to a line of type. The grid is ruled paper rather than an arbitrary
  // lattice, so it is measured from the font rather than guessed at.
  SetGridBaseStep(text::LineHeight(fonts_, Face::Mono, kDefaultBlockEm));

  viewport_.Reset({0.0f, 0.0f}, kDefaultZoom);
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

void App::Shutdown() {
  // The blocks go first, and by hand: a shader has to be released while the
  // graphics context still exists, and destructors run after it is gone.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->ReleaseGpu();
  // Pictures hold a texture each and a video holds a decoder with a child
  // process behind it. Both have to go while there is still a context to hand
  // them back to.
  media_.ReleaseGpu();
  if (cursorHidden_) {
    ShowCursor();
    cursorHidden_ = false;
  }
  fonts_.Unload();
}

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
                             plots_.DockedBounds(block, viewport_, fonts_),
                             shaders_.DockedBounds(block, viewport_, fonts_)};
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
  // What lands on the grid is the text, not the card's corner. There is one dot
  // per line of type, so a box put down this way has its lines sitting in the
  // gaps between rows of dots; snapping the corner instead would put them
  // wherever the padding happened to leave them.
  const Vector2 pad = block->TextOffset();
  const Vector2 at =
      sb::SnapToGrid({desired.x + pad.x, desired.y + pad.y}, viewport_.Zoom());
  block->SetWorldPosition({at.x - pad.x, at.y - pad.y});
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
    if (GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f) dragBlockMoved_ = true;
    const bool absorbing = dragBlockMoved_ && OverDropTarget(pointer);
    dragBlock_->SetAbsorbed(absorbing);

    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
      // While it is on the target it stops moving as well as stops being drawn:
      // it is being held over the cross, not placed anywhere.
      if (!absorbing) {
        const Vector2 world = viewport_.ToWorld(pointer);
        PlaceBlock(dragBlock_, {world.x - dragBlockGrab_.x, world.y - dragBlockGrab_.y});
      }
    } else {
      CodeBlock* dropped = dragBlock_;
      dropped->SetDragging(false);
      dropped->SetAbsorbed(false);
      dragBlock_ = nullptr;
      dragBlockMoved_ = false;
      if (absorbing) RemoveBlock(dropped);
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
    dragBlockMoved_ = false;
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
  if (textBlock_ == block) {
    textBlock_ = nullptr;
    textDrag_ = TextDrag::None;
  }
  axes_.Forget(block);
  plots_.Forget(block);
  shaders_.Forget(block);
  block->ReleaseGpu();
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
  codeBlocks_.back()->SetVizSources(&vizes_.Sources());
  CodeBlock* block = codeBlocks_.back().get();
  // The middle of the screen may well be occupied; the new box goes to the
  // nearest free dot instead of opening on top of what is there.
  PlaceBlock(block, world);
  return block;
}

// ------------------------------------------------------------------ selection

void App::ClearSelection() {
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->SetSelected(false);
  axes_.ClearSelection();
  plots_.ClearSelection();
  shaders_.ClearSelection();
  media_.ClearSelection();
}

bool App::AnySelected() const {
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (block->Selected()) return true;
  }
  return axes_.AnySelected() || plots_.AnySelected() || shaders_.AnySelected() ||
         media_.AnySelected();
}

bool App::SelectedRimAt(Vector2 point) {
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (block->Selected() && block->OnRim(point, viewport_, fonts_)) return true;
  }
  return axes_.SelectedRimAt(point, viewport_, fonts_) ||
         plots_.SelectedRimAt(point, viewport_, fonts_) ||
         shaders_.SelectedRimAt(point, viewport_, fonts_) ||
         media_.SelectedRimAt(point, viewport_);
}

void App::DeleteSelection() {
  axes_.DeleteSelection();
  plots_.DeleteSelection();
  shaders_.DeleteSelection();
  media_.DeleteSelection();

  // Boxes last: removing one takes its windows with it, and the panels have
  // already had their say about the ones that were selected in their own right.
  for (std::size_t i = 0; i < codeBlocks_.size();) {
    if (codeBlocks_[i]->Selected()) {
      RemoveBlock(codeBlocks_[i].get());
      continue;   // the vector shifted under us
    }
    ++i;
  }
}

void App::CopySelection(bool cut) {
  clipboard_.clear();

  bool  any = false;
  Vector2 corner{0.0f, 0.0f};
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (!block->Selected()) continue;
    const Vector2 origin = block->WorldPosition();
    if (!any) { corner = origin; any = true; continue; }
    corner = {std::min(corner.x, origin.x), std::min(corner.y, origin.y)};
  }
  if (!any) return;

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (!block->Selected()) continue;
    Clipping clip;
    clip.lines = block->Lines();
    clip.em = block->Em();
    clip.showMath = block->ShowMath();
    clip.bold = block->Bold();
    clip.italic = block->Italic();
    clip.justify = static_cast<int>(block->Justification());
    clip.textWidth = block->TextWidth();
    clip.showSource = block->ShowSource();
    const Vector2 origin = block->WorldPosition();
    clip.offset = {origin.x - corner.x, origin.y - corner.y};
    // The knobs come along: a copy that started from zero would not be a copy.
    for (const mathprogram::Variable& variable : block->Variables()) {
      if (variable.free && variable.kind == mathprogram::Kind::Real) {
        clip.values.emplace_back(variable.name, variable.value);
      }
    }
    clipboard_.push_back(std::move(clip));
  }

  if (cut) DeleteSelection();
}

void App::PasteClipboard() {
  if (clipboard_.empty()) return;

  // Where you are pointing is where it lands.
  const Vector2 anchor = sb::SnapToGrid(viewport_.ToWorld(GetMousePosition()), viewport_.Zoom());
  ClearSelection();

  for (const Clipping& clip : clipboard_) {
    auto block = std::make_unique<CodeBlock>(
        Vector2{anchor.x + clip.offset.x, anchor.y + clip.offset.y}, clip.em, clip.lines);
    block->SetShowMath(clip.showMath);
    block->SetBold(clip.bold);
    block->SetItalic(clip.italic);
    block->SetJustification(static_cast<CodeBlock::Justify>(clip.justify));
    block->SetTextWidth(clip.textWidth);
    block->SetShowSource(clip.showSource);
    block->RestoreValues(clip.values);
    block->RequestCompile();
    // Selected as they arrive, so the paste can be moved straight where it is
    // wanted without hunting for it.
    block->SetSelected(true);
    codeBlocks_.push_back(std::move(block));
    PlaceBlock(codeBlocks_.back().get(), codeBlocks_.back()->WorldPosition());
  }
}

void App::HandleSelection() {
  const Vector2 pointer = GetMousePosition();

  // --- moving everything selected at once ----------------------------------
  if (selectionDrag_) {
    const Vector2 world = viewport_.ToWorld(pointer);
    const Vector2 wanted = {world.x - selectionGrab_.x, world.y - selectionGrab_.y};
    // Snapped as one vector rather than per window, so the arrangement inside
    // the selection survives the move exactly and everything stays on the grid.
    const Vector2 delta = sb::SnapToGrid(wanted, viewport_.Zoom());
    if (delta.x != 0.0f || delta.y != 0.0f) selectionMoved_ = true;

    const bool absorbing = selectionMoved_ && OverDropTarget(pointer);
    for (auto& [block, from] : selectionFrom_) {
      block->SetAbsorbed(absorbing);
      if (!absorbing) block->SetWorldPosition({from.x + delta.x, from.y + delta.y});
    }
    media_.SetSelectionAbsorbed(absorbing);
    if (!absorbing) {
      axes_.MoveSelection(delta);
      plots_.MoveSelection(delta);
      shaders_.MoveSelection(delta);
      media_.MoveSelection(delta);
    }

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      for (auto& [block, from] : selectionFrom_) block->SetAbsorbed(false);
      selectionDrag_ = false;
      selectionMoved_ = false;
      selectionFrom_.clear();
      if (absorbing) {
        DeleteSelection();
      } else {
        // The group moved rigidly and may have landed on something that was not
        // part of it; the canvas puts that right once the gesture is over.
        SettleBlocks();
      }
    }
    return;
  }

  // --- starting one --------------------------------------------------------
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && AnySelected() && SelectedRimAt(pointer)) {
    selectionDrag_ = true;
    selectionMoved_ = false;
    selectionGrab_ = viewport_.ToWorld(pointer);
    selectionFrom_.clear();
    for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
      if (block->Selected()) selectionFrom_.emplace_back(block.get(), block->WorldPosition());
    }
    axes_.BeginSelectionDrag();
    plots_.BeginSelectionDrag();
    shaders_.BeginSelectionDrag();
    media_.BeginSelectionDrag();
    return;
  }

  // --- drawing the lasso ---------------------------------------------------
  if (lassoActive_) {
    if (lasso_.empty() ||
        std::hypot(pointer.x - lasso_.back().x, pointer.y - lasso_.back().y) > kLassoStep) {
      lasso_.push_back(pointer);
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      // A loop that never went anywhere is a click on the background, not a
      // selection: it puts the caret down rather than picking anything up. The
      // press started this lasso, so HandlePointer never saw it and this is
      // where that click has to be answered.
      //
      // How far it *got*, not where it ended: a lasso that comes back to its
      // own start is the ordinary case, and would otherwise read as a click.
      float reach = 0.0f;
      for (const Vector2& point : lasso_) {
        reach = std::max(reach,
                         std::hypot(point.x - lasso_.front().x, point.y - lasso_.front().y));
      }
      const bool wentNowhere = lasso_.size() <= 1 || reach <= kClickSlop;
      if (wentNowhere) {
        SetFocus(nullptr);
      } else {
        // The loop as it was drawn *and closed*: the curve that bridged the gap
        // is part of the shape, so what is caught is what you watched yourself
        // circle rather than what a chord would have caught.
        const std::vector<Vector2> loop = ClosedLasso(lasso_);
        axes_.SelectWithin(loop, viewport_, fonts_);
        plots_.SelectWithin(loop, viewport_, fonts_);
        shaders_.SelectWithin(loop, viewport_, fonts_);
        media_.SelectWithin(loop, viewport_);
        for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
          block->SetSelected(LassoCatches(loop, block->Measure(viewport_, fonts_).rect));
        }
      }
      lassoActive_ = false;
      lasso_.clear();
    }
    return;
  }

  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && BlockAt(pointer) == nullptr &&
      media_.ItemAt(pointer, viewport_) < 0) {
    // The background, and nothing else, is what the lasso is drawn on.
    lassoActive_ = true;
    lasso_.clear();
    lasso_.push_back(pointer);
  }
}

bool App::HandleResize() {
  const float wheel = GetMouseWheelMoveV().y;
  if (wheel == 0.0f) return false;

  const Vector2 pointer = GetMousePosition();
  // Pictures are drawn over the boxes, so they are asked first.
  const int  item = media_.ItemAt(pointer, viewport_);
  CodeBlock* block = (item >= 0) ? nullptr : BlockAt(pointer);
  if (item < 0 && block == nullptr) return false;

  const Rectangle mine =
      (item >= 0) ? media_.WorldBounds(item) : block->WorldBounds(viewport_, fonts_);
  const float   asked = std::pow(kResizeStep, wheel);
  const Vector2 anchor = NearestCorner(mine, viewport_.ToWorld(pointer));

  // A picture or a piece of prose snaps to the size of what is near it. A code
  // box does not: "the same size as that one" says something about two pictures
  // side by side and nothing at all about two programs.
  float scale = asked;
  if (item >= 0 || block->IsText()) {
    std::vector<Rectangle> neighbours;
    for (int i = 0; i < static_cast<int>(media_.Items().size()); ++i) {
      if (i != item) neighbours.push_back(media_.WorldBounds(i));
    }
    for (const std::unique_ptr<CodeBlock>& other : codeBlocks_) {
      if (other.get() != block) neighbours.push_back(other->WorldBounds(viewport_, fonts_));
    }
    scale = SnapResizeScale(mine, asked, neighbours);
  }

  if (item >= 0) {
    media_.Scale(item, scale, anchor);
  } else {
    block->Scale(scale, anchor);
    // A box that just grew may be sitting on its neighbour, and boxes do not
    // overlap however they came to.
    ResolveOverlaps(block);
  }
  return true;
}

void App::HandleDroppedFiles() {
  if (!IsFileDropped()) return;
  const FilePathList dropped = LoadDroppedFiles();

  const Vector2 at = viewport_.ToWorld(GetMousePosition());
  float         offset = 0.0f;
  for (unsigned int i = 0; i < dropped.count; ++i) {
    const std::string path = dropped.paths[i];
    if (ClassifyMedia(path, nullptr)) {
      // Where you dropped it is where it lands, on the grid like everything
      // else. Several at once are stepped apart rather than stacked.
      const Vector2 corner =
          sb::SnapToGrid({at.x + offset, at.y + offset}, viewport_.Zoom());
      if (media_.Add(path, corner)) offset += 48.0f;
      continue;
    }
    // A canvas dropped on a canvas opens it, which is the obvious reading.
    if (path.size() > 7 && path.compare(path.size() - 7, 7, ".sketch") == 0) {
      if (!LoadCanvas(path)) TraceLog(LOG_WARNING, "CANVAS: could not load %s", path.c_str());
      break;   // the canvas underneath us has just been replaced
    }
  }
  UnloadDroppedFiles(dropped);
}


std::vector<Vector2> App::WindowsOf(const CodeBlock* block, const std::string& name) {
  std::vector<Vector2> found;
  Vector2              at{};
  if (axes_.MarkerFor(block, name, viewport_, fonts_, &at)) found.push_back(at);
  if (plots_.MarkerFor(block, name, viewport_, fonts_, &at)) found.push_back(at);
  return found;
}


// ------------------------------------------------- two values, held together
namespace {

// How close the two fingers have to come before they count as having met.
//
// Proportional to how far apart they started, because "bringing them together"
// is a relative gesture and screens differ. Two fingertips side by side already
// report their centres a good forty or fifty pixels apart, so a threshold in
// the tens is one no hand can ever reach: this asks for the distance to be
// halved, with a floor and a ceiling so that starting very close or very far
// apart still means something sensible.
float ComposeMeet(float startedApart) {
  return std::clamp(startedApart * 0.5f, 90.0f, 200.0f);
}

constexpr float kComposeButton = 60.0f;
constexpr float kComposeGap = 10.0f;
constexpr const char* const kComposeOps[] = {"+", "*"};
constexpr int kComposeOpCount = 2;

Rectangle ComposeButtonRect(Vector2 at, int index) {
  const float width = kComposeOpCount * kComposeButton + (kComposeOpCount + 1) * kComposeGap;
  const float left = at.x - width * 0.5f;
  return {left + kComposeGap + static_cast<float>(index) * (kComposeButton + kComposeGap),
          at.y - kComposeButton * 0.5f, kComposeButton, kComposeButton};
}

Rectangle ComposePanelRect(Vector2 at) {
  const float width = kComposeOpCount * kComposeButton + (kComposeOpCount + 1) * kComposeGap;
  const float height = kComposeButton + 2.0f * kComposeGap;
  return {at.x - width * 0.5f, at.y - height * 0.5f, width, height};
}

float Apart(Vector2 a, Vector2 b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

void App::ComposeInto(const std::string& op) {
  if (composeBlock_ == nullptr) return;
  const mathprogram::Variable* left = composeBlock_->FindVariable(composeNames_[0]);
  const mathprogram::Variable* right = composeBlock_->FindVariable(composeNames_[1]);
  if (left == nullptr || right == nullptr) return;

  // A function anywhere in it makes the answer a function; two reals make a
  // real. Nothing else has a viz to have been touched.
  const bool function = left->kind == mathprogram::Kind::Function ||
                        right->kind == mathprogram::Kind::Function;
  const std::string type = function ? std::string(types::realfunction::kName) : "float";
  const std::string name = composeBlock_->FreshName();
  composeBlock_->InsertDeclaration(type + " " + name + " = " + composeNames_[0] + " " + op +
                                   " " + composeNames_[1] + ";");
}

bool App::HandleCompose() {
  const int           count = std::min(touch::Count(), touch::kMaxPoints);
  const touch::Point* points = touch::Points();

  // ---- the menu, once the fingers have met --------------------------------
  if (compose_ == Compose::Menu) {
    // A finger has to stay down to choose with: the menu belongs to the gesture
    // that raised it, and letting go of the gesture puts it away. Whichever
    // contact is still down is the one pointing, wherever it now is.
    composeHot_ = -1;
    if (count > 0) {
      composeTip_ = points[0].position;
      for (int i = 0; i < kComposeOpCount; ++i) {
        if (CheckCollisionPointRec(composeTip_, ComposeButtonRect(composeMenu_, i))) {
          composeHot_ = i;
        }
      }
      return true;
    }

    // Everything lifted. Where the last finger was is what it chose, and
    // nowhere in particular chooses nothing.
    int chosen = -1;
    for (int i = 0; i < kComposeOpCount; ++i) {
      if (CheckCollisionPointRec(composeTip_, ComposeButtonRect(composeMenu_, i))) chosen = i;
    }
    if (chosen >= 0) ComposeInto(kComposeOps[chosen]);
    compose_ = Compose::None;
    return true;
  }

  // ---- two fingers already down -------------------------------------------
  if (compose_ == Compose::Holding) {
    // Follow each finger, by its own id where that still exists and by where it
    // was otherwise. A digitizer asked to track two fingertips that are nearly
    // touching will happily drop one and hand the other back under a new id --
    // which is to say it does this exactly when the gesture is succeeding.
    bool held[2] = {false, false};
    bool taken[touch::kMaxPoints] = {};
    for (int k = 0; k < 2; ++k) {
      for (int i = 0; i < count; ++i) {
        if (taken[i] || points[i].id != composeIds_[k]) continue;
        composeAt_[k] = points[i].position;
        taken[i] = true;
        held[k] = true;
        break;
      }
    }
    for (int k = 0; k < 2; ++k) {
      if (held[k]) continue;
      for (int i = 0; i < count; ++i) {
        if (taken[i] || Apart(points[i].position, composeAt_[k]) > 90.0f) continue;
        composeIds_[k] = points[i].id;
        composeAt_[k] = points[i].position;
        taken[i] = true;
        held[k] = true;
        break;
      }
    }

    // Held means frozen: whatever else would have moved these values does not
    // get the chance to.
    axes_.AbandonDrag();

    // Asked before the contacts are counted, so that losing one at the very
    // moment they met still counts as having met.
    if (Apart(composeAt_[0], composeAt_[1]) <= ComposeMeet(composeApart_)) {
      composeMenu_ = {(composeAt_[0].x + composeAt_[1].x) * 0.5f,
                      (composeAt_[0].y + composeAt_[1].y) * 0.5f};
      composeTip_ = composeMenu_;
      composeHot_ = -1;
      compose_ = Compose::Menu;
      return true;
    }
    if (!held[0] || !held[1]) {
      // A finger lifted before they met: nothing was meant by it.
      compose_ = Compose::None;
      return false;
    }
    return true;
  }

  // ---- starting one --------------------------------------------------------
  if (count != 2) return false;

  CodeBlock*  block = nullptr;
  std::string names[2];
  for (int i = 0; i < 2; ++i) {
    AxisRef  marker;
    CurveRef curve;
    if (axes_.ValueAt(points[i].position, viewport_, fonts_, &marker)) {
      block = (block == nullptr) ? marker.block : block;
      if (marker.block != block) return false;
      names[i] = marker.name;
    } else if (plots_.ValueAt(points[i].position, viewport_, fonts_, &curve)) {
      block = (block == nullptr) ? curve.block : block;
      if (curve.block != block) return false;
      names[i] = curve.name;
    } else {
      return false;   // a finger on nothing: this is a pinch, not a gesture
    }
  }
  // Two values of the same box, and two different ones. Values from different
  // boxes are values of different programs, and no line could name them both.
  if (block == nullptr || names[0].empty() || names[1].empty() || names[0] == names[1]) {
    return false;
  }

  // Whatever one finger had started doing to a handle is undone: it had not
  // chosen anything yet, and what the two fingers mean is something else.
  axes_.AbandonDrag();

  compose_ = Compose::Holding;
  composeBlock_ = block;
  for (int i = 0; i < 2; ++i) {
    composeNames_[i] = names[i];
    composeIds_[i] = points[i].id;
    composeAt_[i] = points[i].position;
  }
  // How far apart they began, since what counts as together is relative to it.
  composeApart_ = Apart(composeAt_[0], composeAt_[1]);
  return true;
}

void App::DrawCompose() {
  if (compose_ == Compose::None || composeBlock_ == nullptr) return;

  const float thickness = std::max(1.6f, 2.0f * viewport_.Zoom());
  const auto  cable = [&](Vector2 a, Vector2 b, Color tint) {
    const float reach = std::clamp(std::abs(b.x - a.x) * 0.45f, 24.0f, 190.0f);
    const float direction = (b.x > a.x) ? 1.0f : -1.0f;
    DrawSplineSegmentBezierCubic(a, {a.x + reach * direction, a.y},
                                 {b.x - reach * direction, b.y}, b, thickness, tint);
  };

  // A wire back to where each value actually is, so that what is being held is
  // never in doubt -- the finger is only where your hand is.
  const Vector2 ends[2] = {composeAt_[0], composeAt_[1]};
  for (int i = 0; i < 2; ++i) {
    Vector2 from{};
    if (!axes_.MarkerFor(composeBlock_, composeNames_[i], viewport_, fonts_, &from) &&
        !plots_.MarkerFor(composeBlock_, composeNames_[i], viewport_, fonts_, &from)) {
      continue;
    }
    const Vector2 to = (compose_ == Compose::Menu) ? composeMenu_ : ends[i];
    cable(from, to, Fade(composeBlock_->VariableColor(composeNames_[i]), 0.9f));
  }

  if (compose_ != Compose::Menu) return;

  // And where they met, the two things they could be made into.
  const Rectangle panel = ComposePanelRect(composeMenu_);
  DrawRectangleRounded(panel, 0.3f, 10, kOperatorFill);
  DrawRectangleRoundedLinesEx(panel, 0.3f, 10, 1.5f, kOperatorEdge);

  for (int i = 0; i < kComposeOpCount; ++i) {
    const Rectangle box = ComposeButtonRect(composeMenu_, i);
    const bool      hot = (composeHot_ == i);
    DrawRectangleRounded(box, 0.3f, 8, hot ? kAccentHover : kAccent);
    const float em = kComposeButton * 0.5f;
    const float width = text::Measure(fonts_, Face::Mono, kComposeOps[i], em);
    text::Draw(fonts_, Face::Mono, kComposeOps[i],
               {box.x + (box.width - width) * 0.5f, box.y + box.height * 0.5f + em * 0.36f}, em,
               RAYWHITE);
  }
}

bool App::CanWire(const ActiveValue& active) {
  if (active.block == nullptr) return false;
  const mathline::Flow flow = active.block->FlowFor(active.name);
  if (!flow.Interesting()) return false;
  for (const mathline::FlowNode& node : flow.nodes) {
    if (node.op.empty() && !WindowsOf(active.block, node.name).empty()) return true;
  }
  return false;
}

bool App::DrawWiring(const ActiveValue& active) {
  if (active.block == nullptr) return false;
  const mathline::Flow flow = active.block->FlowFor(active.name);
  if (!flow.Interesting()) return false;

  // Where each leaf comes from: its own window, not the place its name is
  // written. The window *is* the value; a mention is only where it is spoken of.
  std::vector<Vector2> where(flow.nodes.size(), Vector2{0.0f, 0.0f});
  std::vector<bool>    known(flow.nodes.size(), false);
  for (std::size_t i = 0; i < flow.nodes.size(); ++i) {
    if (!flow.nodes[i].op.empty()) continue;
    const std::vector<Vector2> windows = WindowsOf(active.block, flow.nodes[i].name);
    if (windows.empty()) continue;
    where[i] = windows.front();
    known[i] = true;
  }

  // Where it all ends up: every place the result is written, and its own
  // windows too.
  std::vector<Vector2> sinks;
  for (const Rectangle& box : active.block->MentionsOf(flow.result, viewport_, fonts_)) {
    sinks.push_back({box.x, box.y + box.height * 0.5f});
  }
  const std::vector<Vector2> resultWindows = WindowsOf(active.block, flow.result);
  for (const Vector2& window : resultWindows) sinks.push_back(window);
  if (sinks.empty()) return false;

  Vector2 target{0.0f, 0.0f};
  for (const Vector2& sink : sinks) { target.x += sink.x; target.y += sink.y; }
  target.x /= static_cast<float>(sinks.size());
  target.y /= static_cast<float>(sinks.size());

  // Where the machine is drawn: under the box and everything docked to it, in
  // the free canvas. Not between the values and the result, tempting as that
  // is -- a number line sits right under its box, so "between" is a few pixels
  // and the boxes would land on top of the very markers they are wired to.
  const Rectangle card = active.block->Measure(viewport_, fonts_).rect;
  float           baseY = card.y + card.height;
  for (const Rectangle& docked : {axes_.DockedBounds(active.block, viewport_, fonts_),
                                  plots_.DockedBounds(active.block, viewport_, fonts_)}) {
    if (docked.height > 0.0f) baseY = std::max(baseY, docked.y + docked.height);
  }
  for (const Vector2& sink : sinks) baseY = std::max(baseY, sink.y);
  baseY += kOperatorHeight * 1.4f;
  const float rung = kOperatorHeight * 2.1f;

  // How deep each operator is above the values it is made of, so the chain has
  // a rung of its own. Barycentric across, so a box sits between what feeds it.
  std::vector<int> depth(flow.nodes.size(), 0);
  int              deepest = 0;
  for (std::size_t i = 0; i < flow.nodes.size(); ++i) {
    const mathline::FlowNode& node = flow.nodes[i];
    if (node.op.empty()) continue;
    int below = 0;
    if (node.lhs >= 0) below = std::max(below, depth[static_cast<std::size_t>(node.lhs)]);
    if (node.rhs >= 0) below = std::max(below, depth[static_cast<std::size_t>(node.rhs)]);
    depth[i] = below + 1;
    deepest = std::max(deepest, depth[i]);
  }

  for (std::size_t i = 0; i < flow.nodes.size(); ++i) {
    const mathline::FlowNode& node = flow.nodes[i];
    if (node.op.empty()) continue;

    Vector2 mean{0.0f, 0.0f};
    int     count = 0;
    for (const int feed : {node.lhs, node.rhs}) {
      if (feed < 0 || !known[static_cast<std::size_t>(feed)]) continue;
      mean.x += where[static_cast<std::size_t>(feed)].x;
      mean.y += where[static_cast<std::size_t>(feed)].y;
      ++count;
    }
    // An operator with nothing known feeding it has nothing to say.
    if (count == 0) continue;
    mean.x /= static_cast<float>(count);
    // Deepest at the bottom, the last one nearest the box: the working runs
    // *towards* what it makes. Rungs the other way up would have every bullet
    // pointing back over its own inputs.
    where[i] = {mean.x, baseY + static_cast<float>(deepest - depth[i]) * rung};
    known[i] = true;
  }

  if (flow.root < 0 || !known[static_cast<std::size_t>(flow.root)]) return false;

  // Which way each bullet points: at whatever it feeds. A shape with a front
  // says which way the working is going without anything being labelled.
  std::vector<int> parent(flow.nodes.size(), -1);
  for (std::size_t i = 0; i < flow.nodes.size(); ++i) {
    for (const int feed : {flow.nodes[i].lhs, flow.nodes[i].rhs}) {
      if (feed >= 0) parent[static_cast<std::size_t>(feed)] = static_cast<int>(i);
    }
  }

  const float half = kOperatorHeight * 0.5f;
  const auto  unit = [](Vector2 v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y);
    return (length > 1e-4f) ? Vector2{v.x / length, v.y / length} : Vector2{0.0f, -1.0f};
  };

  std::vector<Vector2> forward(flow.nodes.size(), Vector2{0.0f, -1.0f});
  for (std::size_t i = 0; i < flow.nodes.size(); ++i) {
    if (flow.nodes[i].op.empty() || !known[i]) continue;
    const int up = parent[i];
    const Vector2 toward = (up >= 0 && known[static_cast<std::size_t>(up)])
                               ? where[static_cast<std::size_t>(up)]
                               : target;
    forward[i] = unit({toward.x - where[i].x, toward.y - where[i].y});
  }

  // Where a bullet's wires meet it: in at the flat end, out at the point.
  const auto backOf = [&](std::size_t i) {
    return Vector2{where[i].x - forward[i].x * (half + kOperatorPoint * 0.5f),
                   where[i].y - forward[i].y * (half + kOperatorPoint * 0.5f)};
  };
  const auto tipOf = [&](std::size_t i) {
    return Vector2{where[i].x + forward[i].x * (half + kOperatorPoint * 0.5f),
                   where[i].y + forward[i].y * (half + kOperatorPoint * 0.5f)};
  };

  // ---- the wires ----------------------------------------------------------
  const float thickness = std::max(1.4f, 1.8f * viewport_.Zoom());
  const auto  cable = [&](Vector2 a, Vector2 leaving, Vector2 b, Vector2 arriving, Color tint) {
    const float span = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    const float reach = std::clamp(span * 0.4f, 16.0f, 150.0f);
    DrawSplineSegmentBezierCubic(a, {a.x + leaving.x * reach, a.y + leaving.y * reach},
                                 {b.x - arriving.x * reach, b.y - arriving.y * reach}, b,
                                 thickness, tint);
  };

  for (std::size_t i = 0; i < flow.nodes.size(); ++i) {
    const mathline::FlowNode& node = flow.nodes[i];
    if (node.op.empty() || !known[i]) continue;
    const Vector2 into = backOf(i);
    for (const int feed : {node.lhs, node.rhs}) {
      if (feed < 0 || !known[static_cast<std::size_t>(feed)]) continue;
      const std::size_t at = static_cast<std::size_t>(feed);
      const mathline::FlowNode& source = flow.nodes[at];
      // A wire out of a value wears that value's colour; a wire out of a bullet
      // is black, because the working belongs to nothing in particular until it
      // arrives somewhere that does.
      const bool    fromOp = !source.op.empty();
      const Vector2 from = fromOp ? tipOf(at) : where[at];
      const Vector2 leaving =
          fromOp ? forward[at] : Vector2{(into.x > from.x) ? 1.0f : -1.0f, 0.0f};
      const Color tint = fromOp ? kOperatorWire
                                : Fade(active.block->VariableColor(source.name), 0.85f);
      cable(from, leaving, into, forward[i], tint);
    }
  }

  // Out of the last point, one wire -- and only then does it fork. What was
  // made is one thing, however many places it turns up.
  const std::size_t root = static_cast<std::size_t>(flow.root);
  const Vector2     tip = tipOf(root);
  const Vector2     fork = {tip.x + forward[root].x * kOperatorRun,
                            tip.y + forward[root].y * kOperatorRun};
  const Color       resultTint = Fade(active.block->VariableColor(flow.result), 0.85f);
  DrawLineEx(tip, fork, thickness, resultTint);
  for (const Vector2& sink : sinks) {
    const Vector2 leaving{(sink.x > fork.x) ? 1.0f : -1.0f, 0.0f};
    cable(fork, leaving, sink, leaving, resultTint);
  }

  // ---- the bullets --------------------------------------------------------
  const float em = kOperatorHeight * 0.58f;
  for (std::size_t i = 0; i < flow.nodes.size(); ++i) {
    const mathline::FlowNode& node = flow.nodes[i];
    if (node.op.empty() || !known[i]) continue;
    const float width = text::Measure(fonts_, Face::Mono, node.op, em) + 2.0f * kOperatorPadX;
    DrawBullet(where[i], forward[i], width * 0.5f + kOperatorPoint * 0.5f, half, kOperatorFill,
               kOperatorEdge);
    const float glyph = text::Measure(fonts_, Face::Mono, node.op, em);
    text::Draw(fonts_, Face::Mono, node.op,
               {where[i].x - glyph * 0.5f - forward[i].x * kOperatorPoint * 0.35f,
                where[i].y + em * 0.36f - forward[i].y * kOperatorPoint * 0.35f},
               em, kOperatorEdge);
  }
  return true;
}

void App::FurnishSlide(Vector2 centre, const std::string& title,
                       const std::vector<std::string>& body) {
  const float aspect = options_.slideAspect;
  const float em = SlidePanel::TextEm();

  // The sigil is what makes a box prose rather than code. It goes on the first
  // line only, and deleting it is how a box goes back to being code.
  auto prose = [](const std::vector<std::string>& lines) {
    std::vector<std::string> out = lines;
    if (out.empty()) out.emplace_back();
    out[0] = "\\text " + out[0];
    return out;
  };

  // The words run the width of the slide, less its padding and the card's own.
  // A slide is a shape, and its text is set to that shape.
  const float measure = SlidePanel::ContentWidth(aspect) - 2.0f * CodeBlock::PaddingX(em);

  const float zoom = viewport_.Zoom();
  const float step = GridStep(zoom);
  const float margin = 2.0f * kRimThickness / std::max(zoom, 1e-6f);

  auto make = [&](Vector2 where, std::vector<std::string> lines, bool bold) {
    codeBlocks_.push_back(std::make_unique<CodeBlock>(where, em, std::move(lines)));
    CodeBlock* block = codeBlocks_.back().get();
    block->SetBold(bold);
    block->SetTextWidth(measure);
    // Placed, not *put*: slide text goes where the slide says and does not step
    // aside for anything. A title that moved to clear a box would no longer be
    // on its slide -- and because the body runs the full width of the slide,
    // the nearest way clear of a box in the middle of the canvas is upwards,
    // straight off the top of it. Whatever was already there is what moves,
    // which is what the pass at the end of this function does.
    block->SetWorldPosition(sb::SnapToGrid(where, zoom));
    return block;
  };

  const Vector2 titleAt = SlidePanel::TitleAnchor(centre, aspect);
  CodeBlock*    titleBox = make(titleAt, prose({title}), /*bold=*/true);

  // The body sits under the title by however tall the title turned out to be,
  // rather than by a guess: the two must not land on each other, and snapping
  // can move either of them by up to half a dot.
  const Rectangle above = titleBox->WorldBounds(viewport_, fonts_);
  CodeBlock*      bodyBox =
      make({titleAt.x, above.y + above.height + em * 0.6f}, prose(body), /*bold=*/false);
  for (int guard = 0; guard < 8; ++guard) {
    const Vector2 at = bodyBox->WorldPosition();
    if (at.y >= above.y + above.height + margin) break;
    bodyBox->SetWorldPosition({at.x, at.y + step});
  }

  // Now everything that was already on the canvas steps clear of the words.
  for (const std::unique_ptr<CodeBlock>& other : codeBlocks_) {
    if (other.get() != titleBox && other.get() != bodyBox) ResolveOverlaps(other.get());
  }
}

void App::SelectWithinWorld(Rectangle worldRect) {
  const Vector2 topLeft = viewport_.ToScreen({worldRect.x, worldRect.y});
  const Vector2 bottomRight =
      viewport_.ToScreen({worldRect.x + worldRect.width, worldRect.y + worldRect.height});
  // A rectangle is a loop like any other, so this goes through the lasso's own
  // machinery rather than growing a second way of asking the same question.
  const std::vector<Vector2> loop = {topLeft, {bottomRight.x, topLeft.y}, bottomRight,
                                     {topLeft.x, bottomRight.y}};
  axes_.SelectWithin(loop, viewport_, fonts_);
  plots_.SelectWithin(loop, viewport_, fonts_);
  shaders_.SelectWithin(loop, viewport_, fonts_);
  media_.SelectWithin(loop, viewport_);
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    block->SetSelected(LassoCatches(loop, block->Measure(viewport_, fonts_).rect));
  }
}

void App::ThrowAwaySlide(Rectangle bounds, float shift) {
  // What is on the slide goes with it. "On the slide" means the same thing here
  // as "circled" does for the lasso -- the window's middle is inside -- so a
  // caption half over the edge belongs to whichever slide holds its middle.
  ClearSelection();
  SelectWithinWorld(bounds);
  DeleteSelection();

  // The deck is a row, so what was to the right comes back to fill the gap: the
  // slides have already moved themselves, and this is everything on them.
  if (shift > 0.0f) {
    constexpr float kFarRight = 1.0e6f;
    ClearSelection();
    SelectWithinWorld({bounds.x + bounds.width, bounds.y, kFarRight, bounds.height});
    for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
      if (!block->Selected()) continue;
      const Vector2 at = block->WorldPosition();
      block->SetWorldPosition({at.x - shift, at.y});
    }
    axes_.BeginSelectionDrag();
    plots_.BeginSelectionDrag();
    shaders_.BeginSelectionDrag();
    media_.BeginSelectionDrag();
    axes_.MoveSelection({-shift, 0.0f});
    plots_.MoveSelection({-shift, 0.0f});
    shaders_.MoveSelection({-shift, 0.0f});
    media_.MoveSelection({-shift, 0.0f});
  }
  ClearSelection();
  SettleBlocks();
}

bool App::HandleTextGesture() {
  const Vector2 pointer = GetMousePosition();

  // --- a gesture already under way -----------------------------------------
  if (textDrag_ == TextDrag::Selecting && textBlock_ != nullptr) {
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
      textBlock_->ExtendSelection(pointer, viewport_, fonts_);
    } else {
      textDrag_ = TextDrag::None;
      textBlock_ = nullptr;
    }
    return true;
  }
  if (textDrag_ == TextDrag::Arming && textBlock_ != nullptr) {
    const float moved = std::hypot(pointer.x - pullFrom_.x, pointer.y - pullFrom_.y);
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
      // A press that went nowhere was a click on the selection, not a pull.
      textDrag_ = TextDrag::None;
      textBlock_ = nullptr;
    } else if (moved > kClickSlop) {
      // Out it comes, and from here it is an ordinary carried row: it follows
      // the pointer and is dropped wherever the button comes up.
      axes_.BeginPullOut(textBlock_, pullName_, pointer, viewport_);
      textBlock_->SelectNone();
      textDrag_ = TextDrag::None;
      textBlock_ = nullptr;
    }
    return true;
  }

  // --- starting one ---------------------------------------------------------
  if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return false;
  CodeBlock* hit = BlockAt(pointer);
  if (hit == nullptr) return false;

  // Pressing on text that is already selected picks it up rather than throwing
  // the selection away and starting again.
  if (hit == focus_ && hit->PointInSelection(pointer, viewport_, fonts_)) {
    const std::string name = hit->SelectedVariable();
    if (!name.empty()) {
      textDrag_ = TextDrag::Arming;
      textBlock_ = hit;
      pullName_ = name;
      pullFrom_ = pointer;
      return true;
    }
  }

  SetFocus(hit);
  hit->BeginSelection(pointer, viewport_, fonts_);
  textDrag_ = TextDrag::Selecting;
  textBlock_ = hit;
  return true;
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

  // Escape belongs to whatever is being typed into first: cancelling an edit is
  // not a request for the canvas menu.
  if (IsKeyPressed(KEY_ESCAPE) && !media_.Editing()) {
    menu_.Toggle();
    // Opening the menu ends editing, so no caret is left blinking behind it.
    if (menu_.Visible()) SetFocus(nullptr);
  }

  // A press that the menu acted on belongs to the menu, even when acting on it
  // was what closed the menu. Otherwise choosing a recent canvas would put the
  // caret into whatever box of the *new* canvas happened to be under the
  // pointer -- the click would be spent twice, on two different canvases.
  const bool menuOwnsPress = menu_.Visible();

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
    case MenuAction::OpenRecent: {
      const std::string path = menu_.Chosen();
      if (LoadCanvas(path)) {
        menu_.Close();
      } else {
        // It was there when the list was read. Say so rather than doing
        // nothing, since a row that does nothing when pressed is worse than a
        // row that was never offered.
        TraceLog(LOG_WARNING, "CANVAS: could not load %s", path.c_str());
      }
      break;
    }
    case MenuAction::Insert: {
      const std::string path = filedialog::OpenMedia();
      if (!path.empty()) {
        // Into the middle of the view, which is where you are looking.
        const Vector2 middle = viewport_.ToWorld({static_cast<float>(GetScreenWidth()) * 0.5f,
                                                  static_cast<float>(GetScreenHeight()) * 0.5f});
        if (media_.Add(path, sb::SnapToGrid(middle, viewport_.Zoom()))) {
          menu_.Close();
        } else {
          TraceLog(LOG_WARNING, "MEDIA: nothing here can show %s", path.c_str());
        }
      }
      break;
    }
    case MenuAction::None:
      break;
  }

  const bool canvasActive = !menu_.Visible() && !menuOwnsPress;

  // Whatever is already being dragged owns the pointer until the button comes
  // up. Without this a window let go over another window's rim would never see
  // the release, and would stay stuck to the cursor.
  bool consumed = false;
  if (canvasActive) HandleDroppedFiles();

  // A lasso being drawn, or a selection being carried, owns the pointer for the
  // same reason a dragged window does -- and it is the easier one to get wrong,
  // because a lasso is *meant* to sweep across other windows. Hovering a rim
  // claims the pointer, so without this a loop drawn round a box would be
  // cancelled the moment it crossed that box's edge.
  //
  // Reaching for a selected window's rim goes here too, and for the same
  // reason: the rim would otherwise claim the press and carry that one window,
  // when a selection is supposed to move as one.
  const bool selectionInFlight = lassoActive_ || selectionDrag_;
  const bool grabbingSelection = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && AnySelected() &&
                                 SelectedRimAt(GetMousePosition());
  if (canvasActive && (selectionInFlight || grabbingSelection)) {
    HandleSelection();
    consumed = true;
  }

  // A left click **anywhere** is a new intent, so it puts down whatever was
  // selected -- on a window, on a picture, on the bare canvas, it makes no
  // difference. The two exceptions are the press that picks the selection up to
  // carry it, and a gesture already under way. One rule in one place: the
  // windows do not each get an opinion about it.
  if (canvasActive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !selectionInFlight &&
      !grabbingSelection) {
    ClearSelection();
  }
  // Two fingers on two values mean something neither means alone, so this is
  // asked before anything that could move one of them.
  if (canvasActive && !consumed) consumed = HandleCompose();
  if (canvasActive && textDrag_ != TextDrag::None) consumed = HandleTextGesture();
  if (canvasActive && !consumed && dragBlock_ != nullptr) consumed = HandleBlockDrag();
  if (canvasActive && !consumed && media_.Busy()) consumed = media_.HandleInput(viewport_);
  if (canvasActive && !consumed && axes_.Dragging()) {
    consumed = axes_.HandleInput(viewport_, fonts_, codeBlocks_);
  }
  if (canvasActive && !consumed && plots_.Dragging()) {
    consumed = plots_.HandleInput(viewport_, fonts_);
  }
  if (canvasActive && !consumed && shaders_.Dragging()) {
    consumed = shaders_.HandleInput(viewport_, fonts_);
  }

  // The wheel resizes whatever it is over. It is asked early, before the rims
  // start claiming the pointer, because a wheel event is never ambiguous: no
  // drag can be in flight while one arrives.
  if (canvasActive && !consumed) consumed = HandleResize();

  // Otherwise input goes to the innermost thing first: a box's buttons -- the
  // ones on a focused code box, and the ones a text box shows whenever you
  // reach for them -- then the windows, then the canvas.
  if (canvasActive && !consumed) {
    for (auto it = codeBlocks_.rbegin(); it != codeBlocks_.rend() && !consumed; ++it) {
      consumed = (*it)->HandleChrome(viewport_, fonts_);
    }
  }
  if (canvasActive && !consumed) consumed = media_.HandleInput(viewport_);
  if (canvasActive && !consumed) consumed = HandleBlockDrag();
  // After the rim, so grabbing the edge of a box still moves it.
  if (canvasActive && !consumed) consumed = HandleTextGesture();
  if (canvasActive && !consumed) consumed = axes_.HandleInput(viewport_, fonts_, codeBlocks_);
  if (canvasActive && !consumed) consumed = plots_.HandleInput(viewport_, fonts_);
  if (canvasActive && !consumed) consumed = shaders_.HandleInput(viewport_, fonts_);
  if (canvasActive && !consumed) {
    consumed = slides_.HandleInput(viewport_, fonts_, options_.slideAspect, &viewport_);
  }

  if (canvasActive && !consumed) {
    // Starting one: a press on a selected window's rim moves the whole
    // selection, and a press on the background begins a loop.
    HandleSelection();
    if (!lassoActive_ && !selectionDrag_) HandleTyping();
  } else if (!canvasActive) {
    // Only the menu opening abandons a gesture. Something else taking the
    // pointer does not: that is what the in-flight pass above is for.
    lassoActive_ = false;
    textDrag_ = TextDrag::None;
    textBlock_ = nullptr;
  }

  // Keys that act on the selection, and only while no box is being typed into.
  // Not while a selection is being carried: the drag is holding raw pointers to
  // the very boxes these would destroy.
  const bool keysActOnSelection = canvasActive && focus_ == nullptr && !selectionDrag_;
  if (keysActOnSelection && AnySelected() && IsKeyPressed(KEY_DELETE)) {
    DeleteSelection();
  }
  if (keysActOnSelection && ctrl) {
    if (IsKeyPressed(KEY_C)) CopySelection(/*cut=*/false);
    if (IsKeyPressed(KEY_X)) CopySelection(/*cut=*/true);
    if (IsKeyPressed(KEY_V)) PasteClipboard();
  }

  // Once the camera is framing a slide, left and right go through the deck --
  // presenting is navigating, so the keys that turn a page move the camera.
  // Only while nothing is being typed into, since there the arrows are the
  // caret's, and only from a slide: elsewhere they mean nothing yet.
  if (canvasActive && focus_ == nullptr && !ctrl) {
    if (IsKeyPressed(KEY_RIGHT)) slides_.Step(+1, &viewport_, options_.slideAspect);
    if (IsKeyPressed(KEY_LEFT)) slides_.Step(-1, &viewport_, options_.slideAspect);
  }

  slides_.Advance(GetFrameTime());
  // A slide that has just appeared gets its title and its body, as two windows
  // of the canvas rather than as text the slide owns.
  if (const NewSlide fresh = slides_.TakeNewSlide(); fresh.made) {
    FurnishSlide(fresh.centre, "Title", {"Lorem ipsum", "dolor sit amet"});
  }
  if (const SlideRemoval gone = slides_.TakeRemoval(); gone.asked) {
    ThrowAwaySlide(gone.bounds, gone.shift);
  }
  media_.Advance(GetFrameTime());
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->Poll();

  // A box that has just gained (or lost) number lines is a different size than
  // it was a frame ago, so this is the moment to check nobody is sitting on
  // anybody. Only on the change: it is far too much work to do every frame.
  // What the canvas says about how things are drawn. Rebuilt every frame: a
  // viz is not registered, it is written down somewhere and therefore true, and
  // the boxes saying so are edited like any others.
  if (vizes_.Sync(codeBlocks_)) {
    // What the canvas says about drawing has changed, so every box that has a
    // value of one of those types is now a different program.
    for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
      block->SetVizSources(&vizes_.Sources());
      if (block->VizType().empty()) block->RequestCompile();
    }
  }

  bool shapesChanged = axes_.Sync(codeBlocks_);
  shapesChanged = plots_.Sync(codeBlocks_) || shapesChanged;
  shapesChanged = shaders_.Sync(codeBlocks_) || shapesChanged;
  if (shapesChanged) SettleBlocks();

  // A window being hovered or dragged lights up the variable it stands for,
  // wherever that variable is written. Recomputed every frame: it is a property
  // of where the pointer is, not something to remember.
  std::vector<ActiveValue> active;
  if (canvasActive) {
    axes_.ActiveVariables(viewport_, fonts_, &active);
    plots_.ActiveVariables(viewport_, fonts_, &active);
    shaders_.ActiveVariables(viewport_, fonts_, &active);
  }

  // Taking hold of a name in the text is the same statement as taking hold of
  // its window, so it lights up the same things: the name wherever else it is
  // written, the wire, and the viz blinking at the other end. The reverse of
  // pulling a variable out of the code, and the same gesture -- highlight a
  // mention -- read from the other side.
  if (canvasActive && focus_ != nullptr) {
    for (const std::string& name : focus_->HighlightedVariables()) {
      Vector2 at{};
      if (axes_.MarkerFor(focus_, name, viewport_, fonts_, &at) ||
          plots_.MarkerFor(focus_, name, viewport_, fonts_, &at)) {
        active.push_back({focus_, name, at});
      }
    }
  }
  // Pointing at the bare part of a number line is an offer to put a value into
  // the box that axis belongs to. The box answers by blinking a caret where the
  // declaration would land, so the click has somewhere visible to be aimed.
  const CodeBlock* insertInto = canvasActive ? axes_.InsertTarget(viewport_, fonts_) : nullptr;

  // A value made out of other values gets its whole derivation drawn instead of
  // a plain wire from its window -- the wires end at the same names either way,
  // and one cable is the point.
  for (ActiveValue& value : active) value.wired = CanWire(value);
  active_ = active;

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    std::vector<ActiveValue> mine;
    for (const ActiveValue& value : active) {
      if (value.block == block.get()) mine.push_back(value);
    }
    block->SetHighlights(std::move(mine));
    block->SetInsertCaret(block.get() == insertInto);
  }

  // The canvas pans on the right button wherever the pointer is; the wheel and
  // touch belong to whatever window is under it.
  viewport_.Update(canvasActive, !consumed, !axes_.Scrolling());

  // The dots are what a window lands on, so they are only worth drawing while
  // something is being landed: a window carried, a window resized, or the
  // canvas zoomed -- zoom because it is what decides how fine the lattice is.
  // Panning is none of those and leaves the canvas bare, which is how it should
  // look while you are only reading it.
  //
  // A linger afterwards, because a resize is a run of separate wheel clicks and
  // a grid that came and went between them would be a flicker; and a fade
  // either way, because the dots appearing is information rather than an event.
  constexpr double kGridLinger = 0.5;
  constexpr float  kGridFadeSeconds = 0.16f;
  const bool carrying = (dragBlock_ != nullptr && dragBlockMoved_) ||
                        (selectionDrag_ && selectionMoved_) || axes_.Carrying() ||
                        plots_.Carrying() || shaders_.Carrying() || media_.Carrying();
  const bool scaling = GetMouseWheelMoveV().y != 0.0f || touch::Count() >= 2;
  if (canvasActive && (carrying || scaling)) gridUntil_ = GetTime() + kGridLinger;
  const float wanted = GetTime() < gridUntil_ ? 1.0f : 0.0f;
  const float rate = std::max(GetFrameTime(), 0.0f) / kGridFadeSeconds;
  gridShow_ = (wanted > gridShow_) ? std::min(wanted, gridShow_ + rate)
                                   : std::max(wanted, gridShow_ - rate);

  // While the canvas is being dragged the pointer is a closed hand. The system
  // cursor goes away and ours is drawn in its place, since there is no
  // grabbing-hand cursor to ask the window system for. Losing the window counts
  // as letting go, so the cursor can never be left hidden under another app.
  const bool grabbing = viewport_.PanningWithMouse() && IsWindowFocused();
  if (grabbing != cursorHidden_) {
    if (grabbing) {
      HideCursor();
    } else {
      ShowCursor();
    }
    cursorHidden_ = grabbing;
  }
}

void App::Draw() {
  ClearBackground(kCanvasBg);
  // Everything the rest of this function asks for is off limits to the atlas
  // evictor until the next frame, so nothing can drop a texture another window
  // is still drawing from.
  fonts_.BeginFrame();

  DrawDotGrid(viewport_, gridShow_);

  // Under everything else: a slide is a region of the canvas, and whatever is
  // on it stands on top of it.
  slides_.Draw(viewport_, fonts_, options_.slideAspect);

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->Draw(viewport_, fonts_);
  for (const MathBlock& block : mathBlocks_) block.Draw(viewport_, fonts_);

  axes_.Draw(viewport_, fonts_, &vizes_);
  plots_.Draw(viewport_, fonts_, &vizes_);
  shaders_.Draw(viewport_, fonts_);
  media_.Draw(viewport_, fonts_);

  // Every box's controls, after every box: a strip half under the card next to
  // it is a strip you cannot press.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    block->DrawChrome(viewport_, fonts_);
  }

  // Two values held together, and the choice of what to make of them.
  DrawCompose();

  // Where a value came from, over everything: a wiring diagram chopped in half
  // by a card would say nothing.
  for (const ActiveValue& value : active_) {
    if (value.wired) DrawWiring(value);
  }

  // Error balloons last, so they are never buried under a neighbouring card.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    block->DrawOverlay(viewport_, fonts_);
  }

  // Selected windows wear their rim whether or not they are being pointed at.
  axes_.DrawSelectionRims(viewport_, fonts_);
  plots_.DrawSelectionRims(viewport_, fonts_);
  shaders_.DrawSelectionRims(viewport_, fonts_);
  media_.DrawSelectionRims(viewport_);

  if (lassoActive_) DrawLasso(lasso_);

  // Carrying a window brings up the one place it can be thrown away.
  if (dragBlockMoved_ || selectionMoved_ || axes_.Carrying() || plots_.Carrying() ||
      shaders_.Carrying() || media_.Carrying()) {
    DrawDropTarget(OverDropTarget(GetMousePosition()));
  }

  menu_.Draw(fonts_, options_);

  // Over everything, the menu included: it is the pointer.
  if (cursorHidden_) DrawGrabCursor(GetMousePosition());
}

// ---------------------------------------------------------------- persistence
//
// A plain line-based text format: readable, diffable, and trivial to extend.
// Each record starts with a keyword, and a block's source lines are prefixed
// with "> " so blank lines and leading spaces survive the round trip.


// ------------------------------------------------------------- media paths
//
// A canvas and the pictures on it are one thing, and one thing has to be
// movable. So a file that sits beside the canvas -- or under it -- is written
// down as where it is *relative to the canvas*, and a canvas carried to another
// folder with its pictures still finds them. Anything further afield keeps its
// absolute path, because there is nothing else it could be said relative to.
//
// Hand-rolled rather than std::filesystem::relative, which resolves symlinks and
// touches the disk; this is a question about two strings.
namespace {

std::string FolderOf(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  return (cut == std::string::npos) ? std::string() : path.substr(0, cut);
}

// Same separators throughout, so the two can be compared at all. Forward
// slashes, which Windows takes everywhere and which survive a text file.
std::string Normalised(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  return path;
}

bool IsAbsolute(const std::string& path) {
  if (path.size() >= 2 && path[1] == ':') return true;             // C:/...
  return !path.empty() && (path[0] == '/' || path[0] == '\\');     // /... or UNC
}

// The path to write into a canvas saved at `canvasPath`.
std::string PathForSaving(const std::string& file, const std::string& canvasPath) {
  const std::string folder = Normalised(FolderOf(canvasPath));
  const std::string full = Normalised(file);
  if (folder.empty() || full.size() <= folder.size() + 1) return full;

  // Under the canvas's own folder? Then say so relatively. Compared without
  // case on Windows, where two spellings of the same folder are the same
  // folder.
  for (std::size_t i = 0; i < folder.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(folder[i])) !=
        std::tolower(static_cast<unsigned char>(full[i]))) {
      return full;
    }
  }
  if (full[folder.size()] != '/') return full;
  return full.substr(folder.size() + 1);
}

// And back again, when reading one.
std::string PathForLoading(const std::string& written, const std::string& canvasPath) {
  if (written.empty() || IsAbsolute(written)) return written;
  const std::string folder = FolderOf(canvasPath);
  return folder.empty() ? written : (folder + "/" + written);
}

}  // namespace

bool App::SaveCanvas(const std::string& path) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;

  file << "sketchbook " << kFileVersion << "\n";
  const Vector2 centre = viewport_.Centre();
  file << "view " << centre.x << " " << centre.y << " " << viewport_.Zoom() << "\n";
  file << "aspect " << options_.slideAspect << "\n";
  const Vector2 newSlide = slides_.ButtonPosition();
  file << "newslide " << newSlide.x << " " << newSlide.y << "\n";
  // A slide is only its rim now: its title and its body are text boxes, and so
  // they are saved as boxes along with every other window.
  for (const Slide& slide : slides_.Slides()) {
    file << "slide " << slide.centre.x << " " << slide.centre.y << "\n";
  }

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    const Vector2 position = block->WorldPosition();
    file << "block " << position.x << " " << position.y << " " << block->Em() << " "
         << (block->ShowMath() ? 1 : 0) << " " << (block->Bold() ? 1 : 0) << " "
         << (block->Italic() ? 1 : 0) << " " << static_cast<int>(block->Justification())
         << " " << block->TextWidth() << " " << (block->ShowSource() ? 1 : 0) << "\n";
    for (const std::string& line : block->Lines()) file << "> " << line << "\n";
    for (const mathprogram::Variable& variable : block->Variables()) {
      // Only a real has a value worth keeping. A free function's choice lives in
      // its example generator, which is deterministic in the variable's name.
      if (variable.free && variable.kind == mathprogram::Kind::Real) {
        file << "val " << variable.name << " " << variable.value << "\n";
      }
    }
    for (const std::string& name : block->Hidden()) file << "hide " << name << "\n";
    if (block->ShaderHidden()) file << "hideshader\n";
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

  for (const ShaderView& view : shaders_.Views()) {
    file << "shader " << (view.docked ? 1 : 0) << " " << view.worldPos.x << " "
         << view.worldPos.y << " " << indexOf(view.block) << "\n";
  }

  // Pictures and video. The path goes last and takes the rest of the line,
  // since a path can have spaces in it and the format stays one-record-a-line.
  for (const std::unique_ptr<MediaItem>& item : media_.Items()) {
    file << "media " << static_cast<int>(item->kind) << " " << item->worldPos.x << " "
         << item->worldPos.y << " " << item->worldWidth << " " << item->crop.x << " "
         << item->crop.y << " " << item->crop.width << " " << item->crop.height << " "
         << item->trimStart << " " << item->trimEnd << " "
         << PathForSaving(item->path, path) << "\n";
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
  recent::Remember(path);
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
    bool                                      bold = false;
    bool                                      italic = false;
    int                                       justify = 0;
    float                                     textWidth = 0.0f;
    bool                                      showSource = false;
    std::vector<std::string>                  lines;
    std::vector<std::pair<std::string, float>> values;
    std::vector<std::string>                   hidden;
    bool                                       shaderHidden = false;
  };
  struct LoadedMedia {
    int         kind = 0;
    Vector2     position{};
    float       width = 360.0f;
    Rectangle   crop{0.0f, 0.0f, 1.0f, 1.0f};
    double      trimStart = 0.0;
    double      trimEnd = 0.0;
    std::string path;
  };
  // A canvas written before slides had windows carries its text on the slide
  // record. It is turned into a pair of text boxes on the way in, so an old
  // deck opens as a new one rather than losing its words.
  struct LegacySlideText {
    Vector2                  centre{};
    std::string              title;
    std::vector<std::string> body;
    bool                     any = false;
  };
  struct LoadedShader {
    bool    docked = true;
    Vector2 worldPos{};
    int     block = -1;
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

  std::vector<LoadedBlock>  blocks;
  std::vector<LoadedAxis>   axes;
  std::vector<LoadedPlot>   plots;
  std::vector<LoadedShader> shaderViews;
  std::vector<LoadedMedia>  media;
  std::vector<LegacySlideText> legacy;
  Vector2 centre{0.0f, 0.0f};
  float   zoom = 1.0f;
  bool    inBlock = false;
  float   aspect = kDefaultSlideAspect;
  Vector2 newSlide = SlidePanel::DefaultButtonPosition();
  std::vector<Slide> slides;

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
    } else if (keyword == "block") {
      LoadedBlock block;
      int showMath = 0;
      int bold = 0;
      int italic = 0;
      int justify = 0;
      stream >> block.position.x >> block.position.y >> block.em >> showMath;
      // A file written before text boxes existed simply stops here, and the
      // stream leaves the rest at its defaults.
      int showSource = 0;
      stream >> bold >> italic >> justify >> block.textWidth >> showSource;
      block.showSource = (showSource != 0);
      block.showMath = (showMath != 0);
      block.bold = (bold != 0);
      block.italic = (italic != 0);
      block.justify = std::clamp(justify, 0, 2);
      blocks.push_back(std::move(block));
      inBlock = true;
    } else if (keyword == "val" && inBlock) {
      std::string name;
      float value = 0.0f;
      stream >> name >> value;
      blocks.back().values.emplace_back(name, value);
    } else if (keyword == "aspect") {
      stream >> aspect;
    } else if (keyword == "newslide") {
      stream >> newSlide.x >> newSlide.y;
    } else if (keyword == "slide") {
      Slide slide;
      stream >> slide.centre.x >> slide.centre.y;
      legacy.push_back({slide.centre, {}, {}, false});
      slides.push_back(std::move(slide));
      inBlock = false;
    } else if (keyword == "stitle" && !legacy.empty()) {
      // The rest of the line, verbatim: slide text is prose and has spaces in.
      legacy.back().title = (raw.size() > 7) ? raw.substr(7) : std::string();
      legacy.back().any = true;
    } else if (keyword == "sbody" && !legacy.empty()) {
      legacy.back().body.push_back((raw.size() > 6) ? raw.substr(6) : std::string());
      legacy.back().any = true;
    } else if (keyword == "media") {
      LoadedMedia item;
      stream >> item.kind >> item.position.x >> item.position.y >> item.width >> item.crop.x >>
          item.crop.y >> item.crop.width >> item.crop.height >> item.trimStart >> item.trimEnd;
      // Everything left on the line is the path, spaces and all.
      std::getline(stream, item.path);
      while (!item.path.empty() && item.path.front() == ' ') item.path.erase(0, 1);
      if (!item.path.empty()) media.push_back(std::move(item));
      inBlock = false;
    } else if (keyword == "hide" && inBlock) {
      std::string name;
      stream >> name;
      if (!name.empty()) blocks.back().hidden.push_back(name);
    } else if (keyword == "hideshader" && inBlock) {
      blocks.back().shaderHidden = true;
    } else if (keyword == "shader") {
      LoadedShader view;
      int docked = 1;
      stream >> docked >> view.worldPos.x >> view.worldPos.y >> view.block;
      view.docked = (docked != 0);
      shaderViews.push_back(view);
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

  // Commit. Everything the old canvas held goes first, and by hand where a
  // destructor cannot be trusted to do it: shaders and textures have to be
  // handed back while the graphics context is still there, and every pointer
  // into the blocks has to stop pointing before the blocks stop existing.
  focus_ = nullptr;
  dragBlock_ = nullptr;
  dragBlockMoved_ = false;
  selectionDrag_ = false;
  selectionMoved_ = false;
  selectionFrom_.clear();
  lassoActive_ = false;
  lasso_.clear();
  textDrag_ = TextDrag::None;
  textBlock_ = nullptr;

  compose_ = Compose::None;
  composeBlock_ = nullptr;

  slides_.Reset();
  axes_.Clear();
  plots_.Clear();
  shaders_.Clear();
  // Every picture's texture and every video's decoder, by hand and while the
  // graphics context is still here.
  media_.Clear();
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->ReleaseGpu();
  codeBlocks_.clear();
  mathBlocks_.clear();

  for (LoadedBlock& loaded : blocks) {
    auto block = std::make_unique<CodeBlock>(loaded.position, loaded.em, std::move(loaded.lines));
    block->SetVizSources(&vizes_.Sources());
    block->SetShowMath(loaded.showMath);
    block->SetBold(loaded.bold);
    block->SetItalic(loaded.italic);
    block->SetJustification(static_cast<CodeBlock::Justify>(loaded.justify));
    block->SetTextWidth(loaded.textWidth);
    block->SetShowSource(loaded.showSource);
    block->RestoreHidden(std::move(loaded.hidden));
    block->SetShaderHidden(loaded.shaderHidden);
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

  std::vector<ShaderView> views;
  for (const LoadedShader& loaded : shaderViews) {
    if (loaded.block < 0 || loaded.block >= static_cast<int>(codeBlocks_.size())) continue;
    ShaderView view;
    view.block = codeBlocks_[static_cast<std::size_t>(loaded.block)].get();
    view.docked = loaded.docked;
    view.worldPos = loaded.worldPos;
    views.push_back(view);
  }
  shaders_.Adopt(std::move(views));

  for (const LoadedMedia& loaded : media) {
    media_.Add(PathForLoading(loaded.path, path), loaded.position, loaded.width);
    if (media_.Items().empty()) continue;
    MediaItem& item = *media_.Items().back();
    item.crop = loaded.crop;
    if (loaded.trimEnd > loaded.trimStart) {
      item.trimStart = loaded.trimStart;
      item.trimEnd = loaded.trimEnd;
    }
  }

  options_.slideAspect = (aspect > 0.05f) ? aspect : kDefaultSlideAspect;
  slides_.Adopt(std::move(slides), newSlide);
  viewport_.Reset(centre, zoom);

  // An older canvas kept its slide text on the slide. Give it the two windows
  // it would have if it were made today. After the viewport, because placement
  // is on the grid as drawn.
  for (const LegacySlideText& old : legacy) {
    if (!old.any) continue;
    FurnishSlide(old.centre, old.title.empty() ? "Title" : old.title, old.body);
  }

  // Hold a loaded canvas to the same two rules as a live one. A file saved
  // under them reloads exactly as it was; an older one is tidied on the way in.
  // The zoom has to be restored first, since the lattice depends on it.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    PlaceBlock(block.get(), block->WorldPosition());
  }

  currentPath_ = path;
  recent::Remember(path);
  return true;
}

}  // namespace sb
