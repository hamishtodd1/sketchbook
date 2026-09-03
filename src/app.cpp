#include "app.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

#include "compiler.h"
#include "filedialog.h"
#include "grid.h"
#include "resize.h"
#include "text.h"
#include "theme.h"

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

  // The deck is a column, so what was below comes up to fill the gap: the
  // slides have already moved themselves, and this is everything on them.
  if (shift > 0.0f) {
    constexpr float kFarBelow = 1.0e6f;
    ClearSelection();
    SelectWithinWorld({bounds.x, bounds.y + bounds.height, bounds.width, kFarBelow});
    for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
      if (!block->Selected()) continue;
      const Vector2 at = block->WorldPosition();
      block->SetWorldPosition({at.x, at.y - shift});
    }
    axes_.BeginSelectionDrag();
    plots_.BeginSelectionDrag();
    shaders_.BeginSelectionDrag();
    media_.BeginSelectionDrag();
    axes_.MoveSelection({0.0f, -shift});
    plots_.MoveSelection({0.0f, -shift});
    shaders_.MoveSelection({0.0f, -shift});
    media_.MoveSelection({0.0f, -shift});
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

  const bool canvasActive = !menu_.Visible();

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
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    std::vector<ActiveValue> mine;
    for (const ActiveValue& value : active) {
      if (value.block == block.get()) mine.push_back(value);
    }
    block->SetHighlights(std::move(mine));
  }

  // The canvas pans on the right button wherever the pointer is; the wheel and
  // touch belong to whatever window is under it.
  viewport_.Update(canvasActive, !consumed);

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

  if (options_.showGridDots) DrawDotGrid(viewport_);

  // Under everything else: a slide is a region of the canvas, and whatever is
  // on it stands on top of it.
  slides_.Draw(viewport_, fonts_, options_.slideAspect);

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->Draw(viewport_, fonts_);
  for (const MathBlock& block : mathBlocks_) block.Draw(viewport_, fonts_);

  axes_.Draw(viewport_, fonts_);
  plots_.Draw(viewport_, fonts_);
  shaders_.Draw(viewport_, fonts_);
  media_.Draw(viewport_, fonts_);

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

bool App::SaveCanvas(const std::string& path) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;

  file << "sketchbook " << kFileVersion << "\n";
  const Vector2 centre = viewport_.Centre();
  file << "view " << centre.x << " " << centre.y << " " << viewport_.Zoom() << "\n";
  file << "grid " << (options_.showGridDots ? 1 : 0) << "\n";
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
         << item->trimStart << " " << item->trimEnd << " " << item->path << "\n";
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
  bool    grid = true;
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
    } else if (keyword == "grid") {
      int value = 1;
      stream >> value;
      grid = (value != 0);
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
    media_.Add(loaded.path, loaded.position, loaded.width);
    if (media_.Items().empty()) continue;
    MediaItem& item = *media_.Items().back();
    item.crop = loaded.crop;
    if (loaded.trimEnd > loaded.trimStart) {
      item.trimStart = loaded.trimStart;
      item.trimEnd = loaded.trimEnd;
    }
  }

  options_.showGridDots = grid;
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
  return true;
}

}  // namespace sb
