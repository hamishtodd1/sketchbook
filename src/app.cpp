#include "app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "compiler.h"
#include "filedialog.h"
#include "grid.h"
#include "recent.h"
#include "settings.h"
#include "resize.h"
#include "text.h"
#include "carry.h"
#include "hover.h"
#include "theme.h"
#include "touch.h"
#include "windowname.h"

namespace sb {
namespace {

// The last four characters of a key: enough to say which one is set, and no
// use to anyone who reads them over your shoulder.
std::string KeyTail(const std::string& key) {
  return key.size() < 12 ? std::string() : key.substr(key.size() - 4);
}

// A press that moves less than this is a click, not a drag.
constexpr float kClickSlop = 6.0f;


// Bumped whenever the on-disk shape changes.
constexpr int kFileVersion = 3;

}  // namespace

bool App::Init(const std::string& assetDir, std::string* error) {
  if (!fonts_.Load(assetDir + "/fonts", error)) return false;
  options_.apiKeyTail = KeyTail(settings::ApiKey());
  // Every message to Claude carries the canvas as it is when it is sent.
  chats_.SetCanvasViewer([this]() { return ViewCanvas(); });
  chats_.SetPointedWindow([this](Rectangle* world, std::string* kind) {
    return DwUnder(devPointed_ ? devPointer_ : GetMousePosition(), world, kind);
  });
  // Without the conversations: rewinding a canvas is about the canvas, and must
  // not close a conversation opened since.
  chats_.SetCanvasSnapshotter([this]() {
    std::ostringstream out;
    WriteCanvas(out, currentPath_, /*withView=*/false, /*withChats=*/false);
    return out.str();
  });

  // One dot to a line of type. The grid is ruled paper rather than an arbitrary
  // lattice, so it is measured from the font rather than guessed at.
  SetGridBaseStep(text::LineHeight(fonts_, Face::Mono, kDefaultBlockEm) / kDotsPerLine);

  viewport_.Reset({0.0f, 0.0f}, kDefaultZoom);
  // Everything docked under a box stacks in one column, each kind asking the
  // kinds above it how far down the column has already got.
  plots_.SetAxes(&axes_);
  // And everything a box *makes* flanks it, in this order: right, left, right
  // below the first, left below the second. Each kind only has to know how many
  // slots the kinds before it have taken.
  shaders_.SetBefore({});
  surfaces_.SetBefore({&shaders_});
  volumes_.SetBefore({&shaders_, &surfaces_});
  spaces_.SetBefore({&shaders_, &surfaces_, &volumes_});
  colours_.SetBefore({&shaders_, &surfaces_, &volumes_, &spaces_});

  // Nothing is put on the canvas. It opens empty, because what belongs on it is
  // whatever you are about to put there -- and a starting card is a thing to
  // delete before beginning rather than a thing to begin from. Typing with
  // nothing selected still starts a box wherever you are looking.

  return true;
}

void App::Shutdown() {
  // Before anything is released: writing the canvas down reads the boxes.
  FlushAutosave();
  // The blocks go first, and by hand: a shader has to be released while the
  // graphics context still exists, and destructors run after it is gone.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->ReleaseGpu();
  // Pictures hold a texture each and a video holds a decoder with a child
  // process behind it. Both have to go while there is still a context to hand
  // them back to.
  media_.ReleaseGpu();
  // Every reply in flight stopped, and its thread joined.
  chats_.Clear();
  // And the shared render target, shaders and meshes behind the space windows.
  spaces_.ReleaseGpu();
  colours_.ReleaseGpu();
  axes_.ReleaseGpu();
  plots_.ReleaseGpu();
  // The built-in vizes, which are boxes too, just not on this canvas.
  vizes_.ReleaseGpu();
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
                             shaders_.DockedBounds(block, viewport_, fonts_),
                             colours_.DockedBounds(block, viewport_, fonts_),
                             surfaces_.DockedBounds(block, viewport_, fonts_),
                             volumes_.DockedBounds(block, viewport_, fonts_),
                             spaces_.DockedBounds(block, viewport_, fonts_)};
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
  // The card's top-left corner lands on a dot, as every other window's does --
  // and since a card's padding is one dot, its text lands on one too.
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

  const float step = SnapStep(viewport_.Zoom());
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
  if (pull_.block == block) pull_ = Pull{};
  axes_.Forget(block);
  plots_.Forget(block);
  shaders_.Forget(block);
  colours_.Forget(block);
  surfaces_.Forget(block);
  volumes_.Forget(block);
  spaces_.Forget(block);
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

    // A block the user opened and left completely empty is not content.
    if (previous->Empty() && previous->Variables().empty()) {
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
  // A new box is empty and grows as it is typed into, so it claims room for the
  // first few lines of what is about to be written -- otherwise the gap would be
  // the size of an empty box and the first line typed would run into a
  // neighbour. It starts at that room's top-left, and the room is what is
  // centred.
  const float  em = kDefaultBlockEm;
  const Vector2 room = {text::Measure(fonts_, Face::Mono, "M", em) * 24.0f + em,
                        text::LineHeight(fonts_, Face::Mono, em) * 3.0f + em};
  const Vector2 at = FindGap(room, WindowRects());
  codeBlocks_.push_back(
      std::make_unique<CodeBlock>(at, kDefaultBlockEm, std::vector<std::string>{}));
  CodeBlock* block = codeBlocks_.back().get();
  PlaceBlock(block, at);
  CentreOn({at.x, at.y, room.x, room.y});
  return block;
}

std::vector<Rectangle> App::WindowRects() {
  std::vector<Rectangle> screen;
  axes_.Rects(viewport_, fonts_, &screen);
  plots_.Rects(viewport_, fonts_, &screen);
  shaders_.Rects(viewport_, fonts_, &screen);
  colours_.Rects(viewport_, fonts_, &screen);
  surfaces_.Rects(viewport_, fonts_, &screen);
  volumes_.Rects(viewport_, fonts_, &screen);
  spaces_.Rects(viewport_, fonts_, &screen);
  media_.Rects(viewport_, &screen);
  chats_.Rects(viewport_, &screen);
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (!block->Absorbed() && !block->Empty()) screen.push_back(block->Measure(viewport_, fonts_).rect);
  }
  std::vector<Rectangle> world;
  for (const Rectangle& r : screen) {
    if (r.width <= 0.0f || r.height <= 0.0f) continue;
    const Vector2 a = viewport_.ToWorld({r.x, r.y});
    const Vector2 b = viewport_.ToWorld({r.x + r.width, r.y + r.height});
    world.push_back({a.x, a.y, b.x - a.x, b.y - a.y});
  }
  return world;
}

Vector2 App::FindGap(Vector2 size, const std::vector<Rectangle>& taken) {
  const float   zoom = viewport_.Zoom();
  const Vector2 middle = viewport_.ToWorld(
      {static_cast<float>(GetScreenWidth()) * 0.5f, static_cast<float>(GetScreenHeight()) * 0.5f});
  const Vector2 ideal = SnapToGrid({middle.x - size.x * 0.5f, middle.y - size.y * 0.5f}, zoom);
  // Room round it: a line's height, so two windows never read as one.
  const float room = 2.0f * GridBaseStep();
  const auto  fits = [&](Vector2 at) {
    const Rectangle wanted = {at.x - room, at.y - room, size.x + 2.0f * room, size.y + 2.0f * room};
    for (const Rectangle& t : taken) {
      if (CheckCollisionRecs(wanted, t)) return false;
    }
    return true;
  };
  if (fits(ideal)) return ideal;

  // Outward from the middle, ring by ring, keeping the nearest place that fits
  // and stopping once a ring is further out than it. The lattice is the grid
  // as drawn, or a dot of the base grid where that is finer, which keeps the
  // search to a few thousand tries however far in the camera is.
  const float step = std::max(SnapStep(zoom), GridBaseStep());
  Vector2     best = ideal;
  float       bestDistance = -1.0f;
  constexpr int kRings = 160;
  for (int ring = 1; ring <= kRings; ++ring) {
    if (bestDistance >= 0.0f && static_cast<float>(ring - 1) * step > bestDistance) break;
    for (int i = -ring; i <= ring; ++i) {
      for (int j = -ring; j <= ring; ++j) {
        if (std::max(std::abs(i), std::abs(j)) != ring) continue;
        const Vector2 at = SnapToGrid(
            {ideal.x + static_cast<float>(i) * step, ideal.y + static_cast<float>(j) * step}, zoom);
        const float distance = std::hypot(at.x - ideal.x, at.y - ideal.y);
        if (bestDistance >= 0.0f && distance >= bestDistance) continue;
        if (!fits(at)) continue;
        best = at;
        bestDistance = distance;
      }
    }
  }
  return best;
}

void App::CentreOn(Rectangle world) {
  const Vector2 centre = {world.x + world.width * 0.5f, world.y + world.height * 0.5f};
  const Vector2 middle = viewport_.ToWorld(
      {static_cast<float>(GetScreenWidth()) * 0.5f, static_cast<float>(GetScreenHeight()) * 0.5f});
  // Already there, to within a couple of pixels: nothing to do.
  if (std::hypot(centre.x - middle.x, centre.y - middle.y) * viewport_.Zoom() < 2.0f) return;
  viewport_.GlideTo(centre, viewport_.Zoom(), 0.45f);
}

// ------------------------------------------------------------------ selection

void App::ClearSelection() {
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->SetSelected(false);
  axes_.ClearSelection();
  plots_.ClearSelection();
  shaders_.ClearSelection();
  colours_.ClearSelection();
  surfaces_.ClearSelection();
  volumes_.ClearSelection();
  spaces_.ClearSelection();
  media_.ClearSelection();
  chats_.ClearSelection();
}

bool App::AnySelected() const {
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (block->Selected()) return true;
  }
  return axes_.AnySelected() || plots_.AnySelected() || shaders_.AnySelected() ||
         colours_.AnySelected() ||
         surfaces_.AnySelected() || volumes_.AnySelected() ||
         spaces_.AnySelected() || media_.AnySelected() || chats_.AnySelected();
}

bool App::SelectedRimAt(Vector2 point) {
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (block->Selected() && block->OnRim(point, viewport_, fonts_)) return true;
  }
  return axes_.SelectedRimAt(point, viewport_, fonts_) ||
         plots_.SelectedRimAt(point, viewport_, fonts_) ||
         shaders_.SelectedRimAt(point, viewport_, fonts_) ||
         colours_.SelectedRimAt(point, viewport_, fonts_) ||
         surfaces_.SelectedRimAt(point, viewport_, fonts_) ||
         volumes_.SelectedRimAt(point, viewport_, fonts_) ||
         spaces_.SelectedRimAt(point, viewport_, fonts_) ||
         media_.SelectedRimAt(point, viewport_) || chats_.SelectedRimAt(point, viewport_);
}

void App::DeleteSelection() {
  axes_.DeleteSelection();
  plots_.DeleteSelection();
  shaders_.DeleteSelection();
  colours_.DeleteSelection();
  surfaces_.DeleteSelection();
  volumes_.DeleteSelection();
  spaces_.DeleteSelection();
  media_.DeleteSelection();
  chats_.DeleteSelection();

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
    clip.showMath = block->ReadingMode();
    clip.bold = block->Bold();
    clip.italic = block->Italic();
    clip.justify = static_cast<int>(block->Justification());
    clip.textWidth = block->TextWidth();
    clip.showSource = block->ShowSource();
    const Vector2 origin = block->WorldPosition();
    clip.offset = {origin.x - corner.x, origin.y - corner.y};
    // The knobs come along: a copy that started from zero would not be a copy.
    for (auto& value : block->KnobValues()) clip.values.push_back(std::move(value));
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
    block->SetReadingMode(clip.showMath);
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
    chats_.SetSelectionAbsorbed(absorbing);
    if (!absorbing) {
      axes_.MoveSelection(delta);
      plots_.MoveSelection(delta);
      shaders_.MoveSelection(delta);
      colours_.MoveSelection(delta);
      surfaces_.MoveSelection(delta);
      volumes_.MoveSelection(delta);
      spaces_.MoveSelection(delta);
      media_.MoveSelection(delta);
      chats_.MoveSelection(delta);
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
    colours_.BeginSelectionDrag();
    surfaces_.BeginSelectionDrag();
    volumes_.BeginSelectionDrag();
    spaces_.BeginSelectionDrag();
    media_.BeginSelectionDrag();
    chats_.BeginSelectionDrag();
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
        colours_.SelectWithin(loop, viewport_, fonts_);
        surfaces_.SelectWithin(loop, viewport_, fonts_);
        volumes_.SelectWithin(loop, viewport_, fonts_);
        spaces_.SelectWithin(loop, viewport_, fonts_);
        media_.SelectWithin(loop, viewport_);
        chats_.SelectWithin(loop, viewport_);
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
  undoPending_ = true;
  undoLastInput_ = GetTime();
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
  // A shared value's windows are its home box's windows.
  block = HomeOf(const_cast<CodeBlock*>(block), name);
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
  const std::string name = composeBlock_->FreshName();
  if (!function) {
    composeBlock_->InsertDeclaration("float " + name + " = " + left->name + " " + op + " " +
                                     right->name + ";");
    return;
  }
  // Functions combine by being called, so the answer is a definition. Its
  // parameter is the first of a few letters neither side is called, so a real
  // named x is not shadowed by the x the definition introduces.
  std::string param = "x";
  for (const char* candidate : {"x", "t", "s", "u"}) {
    if (left->name != candidate && right->name != candidate) { param = candidate; break; }
  }
  const auto term = [&](const mathprogram::Variable* v) {
    return v->kind == mathprogram::Kind::Function ? v->name + "(" + param + ")" : v->name;
  };
  composeBlock_->InsertDeclaration("float " + name + "(float " + param + ") { return " +
                                   term(left) + " " + op + " " + term(right) + "; }");
}

void App::Converse(const std::string& text, const std::string& effort) {
  const Vector2 size = ChatPanel::DefaultSize();
  const Vector2 at = FindGap(size, WindowRects());
  chats_.New({at.x + size.x * 0.5f, at.y + size.y * 0.5f}, viewport_.Zoom());
  CentreOn({at.x, at.y, size.x, size.y});
  converseEffort_ = effort;
  std::size_t from = 0;
  while (from <= text.size()) {
    std::size_t split = text.find(" || ", from);
    if (split == std::string::npos) split = text.size();
    converseQueue_.push_back(text.substr(from, split - from));
    from = split + 4;
  }
  ConverseNext();
}

void App::ConverseNext() {
  while (!converseQueue_.empty()) {
    if (chats_.Windows().empty() || chats_.Windows().back()->conversation->Busy()) return;
    if (viewport_.Gliding()) return;   // what is pointed at is where it ends up
    const std::string next = converseQueue_.front();
    converseQueue_.erase(converseQueue_.begin());
    if (next.rfind("point:", 0) == 0) {
      devPointed_ = std::sscanf(next.c_str() + 6, "%f,%f", &devPointer_.x, &devPointer_.y) == 2;
      continue;
    }
    if (next.rfind("box:", 0) == 0) {
      NewBlockAtCentre()->RestoreLines({next.substr(4)});
      continue;
    }
    chats_.Say(next, converseEffort_);
    return;
  }
}

void App::DevChat(const std::string& actions) {
  std::size_t from = 0;
  while (from <= actions.size()) {
    std::size_t comma = actions.find(',', from);
    if (comma == std::string::npos) comma = actions.size();
    const std::string action = actions.substr(from, comma - from);
    if (action == "discard") {
      undoPending_ = true;
      chats_.Dev(action);
      CommitUndo();
    } else if (action == "undo") {
      StepUndo(/*redo=*/false);
    } else if (action == "new") {
      // A conversation, as the menu makes one.
      const Vector2 size = ChatPanel::DefaultSize();
      const Vector2 at = FindGap(size, WindowRects());
      chats_.New({at.x + size.x * 0.5f, at.y + size.y * 0.5f}, viewport_.Zoom());
      CentreOn({at.x, at.y, size.x, size.y});
    } else if (action.rfind("box:", 0) == 0) {
      // A box, as typing on the bare canvas makes one.
      CodeBlock* block = NewBlockAtCentre();
      block->RestoreLines({action.substr(4)});
    } else if (!action.empty()) {
      chats_.Dev(action);
    }
    from = comma + 1;
  }
}

void App::SetAlwaysCode(bool on) {
  options_.alwaysCode = on;
  CodeBlock::SetAlwaysCode(on);
}

void App::SetFullscreen(bool on) {
  if (on == options_.fullscreen) return;
  // Done by hand rather than with raylib's ToggleBorderlessWindowed, which also
  // makes the window *topmost*: alt-tab then brings another app to the front
  // underneath this one, and clicks go to a window you cannot see. Taking the
  // title bar off and covering the monitor is all that filling the screen needs,
  // and an ordinary window can be tabbed away from and back to like any other.
  // No change of display mode, so it is instant and disturbs nothing else.
  if (on) {
    windowedAt_ = GetWindowPosition();
    windowedWidth_ = GetScreenWidth();
    windowedHeight_ = GetScreenHeight();
    const int     monitor = GetCurrentMonitor();
    const Vector2 corner = GetMonitorPosition(monitor);
    SetWindowState(FLAG_WINDOW_UNDECORATED);
    SetWindowPosition(static_cast<int>(corner.x), static_cast<int>(corner.y));
    SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
  } else {
    ClearWindowState(FLAG_WINDOW_UNDECORATED);
    SetWindowSize(windowedWidth_, windowedHeight_);
    SetWindowPosition(static_cast<int>(windowedAt_.x), static_cast<int>(windowedAt_.y));
  }
  options_.fullscreen = on;
  settings::RememberFullscreen(on);
}

bool App::FindPullable(Vector2 at, Pull* out) {
  AxisRef   ref;
  bool      field = false;
  Rectangle window{};
  if (axes_.VizUnder(at, viewport_, fonts_, &ref, &field, &window)) {
    *out = Pull{PullFrom::Axis, ref.block, ref.name, field, window};
    return true;
  }
  if (PointRef point; spaces_.VizUnder(at, viewport_, fonts_, &point, &window)) {
    *out = Pull{PullFrom::Space, point.block, point.name, false, window};
    return true;
  }
  std::size_t plot = 0;
  CurveRef    curve;
  if (plots_.PlotUnder(at, viewport_, fonts_, &plot, &window, &curve)) {
    *out = Pull{PullFrom::Plot, curve.block, curve.name, false, window};
    // One curve on it and there is no question which. Several, and the frame is
    // asked: whatever is drawn under the pointer, drawn last, is the one.
    if (curve.block == nullptr) {
      plots_.AskCurveAt(plot, at, viewport_, fonts_);
      out->picking = true;
    }
    return true;
  }
  return false;
}

bool App::HandlePull() {
  const int           count = std::min(touch::Count(), touch::kMaxPoints);
  const touch::Point* points = touch::Points();
  // Which fingers are new this frame. Remembered on the way out, whatever path
  // is taken, so the next frame compares against this one.
  const auto landed = [&](int i) {
    for (int k = 0; k < touchSeenCount_; ++k) {
      if (touchSeen_[k] == points[i].id) return false;
    }
    return true;
  };
  const auto remember = [&]() {
    touchSeenCount_ = count;
    for (int i = 0; i < count; ++i) touchSeen_[i] = points[i].id;
  };

  // A window a finger pulled out follows that finger, and on the frame it lifts
  // reports one "up" so the panel carrying it drops it. Not a gesture of this
  // function's any more: the panel owns the carry, so this owns no input.
  if (fingerCarry_) {
    bool present = false;
    for (int i = 0; i < count; ++i) {
      if (points[i].id != fingerCarryId_) continue;
      carry::FollowFinger(points[i].position, true);
      present = true;
    }
    if (!present) {
      if (carry::Down(MOUSE_BUTTON_LEFT)) {
        carry::FollowFinger(carry::Position(), false);
      } else {
        carry::FollowMouse();
        fingerCarry_ = false;
      }
    }
    remember();
    return false;
  }

  if (pull_.from != PullFrom::None) {
    Vector2 at = GetMousePosition();
    bool    down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    if (pull_.byFinger) {
      down = false;
      for (int i = 0; i < count; ++i) {
        if (points[i].id != pull_.finger) continue;
        at = points[i].position;
        down = true;
      }
    }
    remember();
    // Let go inside the window: nothing was pulled, and nothing happens.
    if (!down) {
      if (pull_.picking) plots_.ForgetPick();
      pull_ = Pull{};
      return true;
    }
    if (pull_.picking) {
      CurveRef found;
      switch (plots_.CurveAnswer(&found)) {
        case PlotPanel::Pick::Found:
          pull_.block = found.block;
          pull_.name = found.name;
          pull_.picking = false;
          plots_.ForgetPick();
          break;
        case PlotPanel::Pick::None:
          // Pressed on the plot but on no curve of it: there is nothing to pull.
          plots_.ForgetPick();
          pull_ = Pull{};
          return true;
        default:
          return true;   // not drawn yet; it will be by the next frame
      }
    }
    if (CheckCollisionPointRec(at, pull_.window)) return true;
    // Out of the window: from here it is a window of its own, carried by the
    // panel exactly as any window picked up by its rim would be -- which is what
    // makes the drop onto another window a merge without anything new.
    bool began = false;
    if (pull_.from == PullFrom::Axis) {
      began = axes_.BeginPullOut(pull_.block, pull_.name, at, viewport_, pull_.field);
    } else if (pull_.from == PullFrom::Space) {
      began = spaces_.BeginPullOut(pull_.block, pull_.name, at, viewport_);
    } else if (pull_.from == PullFrom::Plot) {
      began = plots_.BeginPullOut(pull_.block, pull_.name, at, viewport_);
    }
    if (began && pull_.byFinger) {
      fingerCarry_ = true;
      fingerCarryId_ = pull_.finger;
      carry::FollowFinger(at, true);
    }
    pull_ = Pull{};
    return true;
  }

  // With a finger: one already down inside a window, and another landing on a
  // viz in that same window. The one standing still is holding the window; the
  // one that just arrived is taking something out of it.
  if (count == 2 && compose_ == Compose::None) {
    int fresh = -1;
    int holding = -1;
    for (int i = 0; i < 2; ++i) (landed(i) ? fresh : holding) = i;
    Pull found;
    if (fresh >= 0 && holding >= 0 && FindPullable(points[fresh].position, &found) &&
        CheckCollisionPointRec(points[holding].position, found.window)) {
      // Whatever the holding finger's emulated press had started on a handle is
      // undone: it was holding the window, not moving a value.
      axes_.AbandonDrag();
      found.byFinger = true;
      found.finger = points[fresh].id;
      pull_ = found;
      remember();
      return true;
    }
  }
  remember();

  // With the mouse: Ctrl and a press on a viz.
  const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
  if (!ctrl || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return false;
  return FindPullable(GetMousePosition(), &pull_);
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

// The spread a type gets when nothing on the canvas says otherwise. A canvas
// with no sampler box still shows a function, exactly as a type with no viz box
// still gets a marker: authoring one is how you take the drawing over, not how
// you switch it on. Only the reals have one, because a spread of reals is the
// one thing the app can guess at -- what a spread of points would be is a
// question only the person drawing it can answer.
namespace {
constexpr int   kBornSamples = 24;
constexpr float kBornLo = -4.0f;
constexpr float kBornHi = 4.0f;
}  // namespace

void App::FeedFunctions() {
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    const int which = block->FunctionShown();
    if (which < 0) continue;
    const mathprogram::Function& fn = block->Functions()[static_cast<std::size_t>(which)];
    if (!fn.sampled) continue;

    // Whatever box on this canvas says how that space is spread out.
    if (const CodeBlock* sampler = vizes_.Sampler(fn.inputType); sampler != nullptr) {
      const int stride = std::max(1, sampler->SamplerStride());
      const std::vector<float>& run = sampler->SamplerRun();
      block->SetSampleInputs(run, static_cast<int>(run.size()) / stride, stride);
      continue;
    }
    if (fn.inStride != 1) continue;   // nothing to guess at

    std::vector<float> spread(kBornSamples, 0.0f);
    for (int i = 0; i < kBornSamples; ++i) {
      spread[static_cast<std::size_t>(i)] =
          kBornLo + (kBornHi - kBornLo) * static_cast<float>(i) /
                        static_cast<float>(kBornSamples - 1);
    }
    block->SetSampleInputs(spread, kBornSamples, 1);
  }
}

// One sample and the answer it turned into, boxed at both ends and joined.
//
// The pointer picks the pair out at either end -- pointing at an answer is as
// good a way of asking about a sample as pointing at the sample is -- and both
// ends light up, because the whole claim being made is that they are two halves
// of one statement. The wire runs over every window rather than under them, for
// the same reason every other wire here does.
void App::DrawSampleLink() {
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    const int lit = block->SampleHover();
    if (lit < 0 || !block->Samples().Live()) continue;

    Vector2 ends[2]{};
    bool    have[2] = {false, false};
    for (int end = 0; end < 2; ++end) {
      const bool output = (end == 1);
      have[end] = axes_.SampleMarker(block.get(), lit, output, viewport_, fonts_, &ends[end]) ||
                  spaces_.SampleMarker(block.get(), lit, output, viewport_, fonts_, &ends[end]);
    }
    if (!have[0] && !have[1]) continue;

    const Color tint = block->VariableColor(block->Samples().name);
    const float zoom = viewport_.Zoom();
    const float side = std::max(9.0f, 11.0f * zoom);
    const float thickness = std::max(1.4f, 1.8f * zoom);

    for (int end = 0; end < 2; ++end) {
      if (!have[end]) continue;
      const Rectangle box = {ends[end].x - side * 0.5f, ends[end].y - side * 0.5f, side, side};
      // Two passes, the way the grabbing cursor is drawn: these land on a dark
      // card, on the pale canvas and on a sky-blue window full of the very dots
      // they are pointing at, and one stroke cannot read on all three.
      const Rectangle halo = {box.x - thickness, box.y - thickness,
                              box.width + thickness * 2.0f, box.height + thickness * 2.0f};
      DrawRectangleLinesEx(halo, thickness, Fade(RAYWHITE, 0.85f));
      DrawRectangleLinesEx(box, thickness, tint);
    }
    if (!have[0] || !have[1]) continue;

    // The same cable every other pair of things on this canvas is joined by:
    // out horizontally at both ends, so it never crosses what it points at.
    const Vector2 a = ends[0];
    const Vector2 b = ends[1];
    const float   reach = std::clamp(std::abs(b.x - a.x) * 0.45f, 20.0f, 160.0f);
    const float   direction = (b.x > a.x) ? 1.0f : -1.0f;
    DrawSplineSegmentBezierCubic(a, {a.x + reach * direction, a.y},
                                 {b.x - reach * direction, b.y}, b, thickness, Fade(tint, 0.9f));
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
  const float step = SnapStep(zoom);
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

void App::FrameOnScreen() {
  std::vector<Rectangle> rects;
  axes_.Rects(viewport_, fonts_, &rects);
  plots_.Rects(viewport_, fonts_, &rects);
  shaders_.Rects(viewport_, fonts_, &rects);
  colours_.Rects(viewport_, fonts_, &rects);
  surfaces_.Rects(viewport_, fonts_, &rects);
  volumes_.Rects(viewport_, fonts_, &rects);
  spaces_.Rects(viewport_, fonts_, &rects);
  media_.Rects(viewport_, &rects);
  chats_.Rects(viewport_, &rects);
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (!block->Absorbed()) rects.push_back(block->Measure(viewport_, fonts_).rect);
  }

  // What is on screen is decided by the middle, as it is for the lasso: a
  // window mostly off the edge is not one you are looking at, and one mostly on
  // it is, however little of it sticks out.
  const float width = static_cast<float>(GetScreenWidth());
  const float height = static_cast<float>(GetScreenHeight());
  bool  any = false;
  float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
  for (const Rectangle& rect : rects) {
    const float cx = rect.x + rect.width * 0.5f;
    const float cy = rect.y + rect.height * 0.5f;
    if (cx < 0.0f || cx > width || cy < 0.0f || cy > height) continue;
    if (!any) {
      left = rect.x; top = rect.y; right = rect.x + rect.width; bottom = rect.y + rect.height;
      any = true;
    } else {
      left = std::min(left, rect.x);
      top = std::min(top, rect.y);
      right = std::max(right, rect.x + rect.width);
      bottom = std::max(bottom, rect.y + rect.height);
    }
  }
  if (!any) return;

  // In world units, which do not depend on the zoom, so the answer is the same
  // whatever zoom it was asked from.
  const Vector2 a = viewport_.ToWorld({left, top});
  const Vector2 b = viewport_.ToWorld({right, bottom});
  const float   spanX = std::max(b.x - a.x, 1.0f);
  const float   spanY = std::max(b.y - a.y, 1.0f);
  // A margin, so what is framed is framed rather than touching the edges.
  constexpr float kMargin = 0.03f;
  const float zoom = std::min(width * (1.0f - 2.0f * kMargin) / spanX,
                              height * (1.0f - 2.0f * kMargin) / spanY);
  viewport_.GlideTo({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}, zoom, 0.45f);
}

CodeBlock* App::HomeOf(CodeBlock* block, const std::string& name) const {
  if (block == nullptr) return nullptr;
  const std::vector<std::string>& taken = block->Program().importedNames();
  if (std::find(taken.begin(), taken.end(), name) == taken.end()) return block;
  const mathprogram::Program* home = nullptr;
  for (const mathprogram::SharedValue& value : canvas_.values) {
    if (value.name == name && value.home != &block->Program()) home = value.home;
  }
  for (const mathprogram::SharedFunction& fn : canvas_.functions) {
    if (fn.name == name && fn.home != &block->Program()) home = fn.home;
  }
  for (const std::unique_ptr<CodeBlock>& other : codeBlocks_) {
    if (&other->Program() == home) return other.get();
  }
  return block;
}

void App::ShareAcrossCanvas() {
  // Every box builds against the canvas, including one made since last frame.
  std::vector<std::pair<const mathprogram::Program*, unsigned long>> now;
  now.reserve(codeBlocks_.size());
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    block->SetCanvas(&canvas_);
    now.emplace_back(&block->Program(), block->Program().sharedVersion());
  }

  // And how the functions on it are drawn: vizes, which a box compiles in.
  std::map<std::string, mathprogram::FunctionViz> drawnWith = vizes_.FunctionVizes(codeBlocks_);

  // Nothing any box offers has changed -- the answer nearly every frame.
  if (now != sharedFrom_ || drawnWith != canvas_.functionVizes) {
    sharedFrom_ = std::move(now);
    std::vector<const mathprogram::Program*> programs;
    programs.reserve(sharedFrom_.size());
    for (const auto& [program, version] : sharedFrom_) programs.push_back(program);
    canvas_ = mathprogram::Gather(programs);
    canvas_.functionVizes = std::move(drawnWith);

    // Only a box whose share of it changed is built again, and a build that
    // comes out the same is not compiled -- so an edit costs the boxes that use
    // what was edited, and nothing else. A box being written in is left alone:
    // it builds when the caret leaves it, and would otherwise compile half a
    // thought.
    for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
      if (block->IsText()) continue;
      if (block->Focused() && block->Dirty()) continue;
      if (block->ImportsStale(canvas_)) block->RequestCompile();
    }
  }

  // A knob in one box moves every box that reads it: its number is fetched
  // here, while the canvas it is fetched through is fresh.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->PullImports(canvas_);
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
  colours_.SelectWithin(loop, viewport_, fonts_);
  surfaces_.SelectWithin(loop, viewport_, fonts_);
  volumes_.SelectWithin(loop, viewport_, fonts_);
  spaces_.SelectWithin(loop, viewport_, fonts_);
  media_.SelectWithin(loop, viewport_);
  chats_.SelectWithin(loop, viewport_);
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
    colours_.BeginSelectionDrag();
    surfaces_.BeginSelectionDrag();
    volumes_.BeginSelectionDrag();
    spaces_.BeginSelectionDrag();
    media_.BeginSelectionDrag();
    chats_.BeginSelectionDrag();
    axes_.MoveSelection({-shift, 0.0f});
    plots_.MoveSelection({-shift, 0.0f});
    shaders_.MoveSelection({-shift, 0.0f});
    colours_.MoveSelection({-shift, 0.0f});
    surfaces_.MoveSelection({-shift, 0.0f});
    volumes_.MoveSelection({-shift, 0.0f});
    spaces_.MoveSelection({-shift, 0.0f});
    media_.MoveSelection({-shift, 0.0f});
    chats_.MoveSelection({-shift, 0.0f});
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
      // the pointer and is dropped wherever the button comes up. A name shared
      // from another box comes out as its home box's value, since that is where
      // its window is.
      axes_.BeginPullOut(HomeOf(textBlock_, pullName_), pullName_, pointer, viewport_);
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
    std::string path = untitled_ ? std::string() : currentPath_;
    if (path.empty()) path = filedialog::Save("canvas.sketch");
    if (!path.empty() && !SaveCanvas(path)) {
      TraceLog(LOG_WARNING, "CANVAS: could not save %s", path.c_str());
    }
  }
  // Undo and redo for the canvas. Inside a box the keys are the box's own, and
  // mid-gesture there is nothing settled to go back to.
  if (ctrl && focus_ == nullptr && !media_.Editing() && !chats_.Editing() && !hover::Held() &&
      !menu_.Visible()) {
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (IsKeyPressed(KEY_Z)) StepUndo(/*redo=*/shift);
    if (IsKeyPressed(KEY_Y)) StepUndo(/*redo=*/true);
  }
  if (ctrl && IsKeyPressed(KEY_O)) {
    const std::string path = filedialog::Open();
    if (!path.empty() && !LoadCanvas(path)) {
      TraceLog(LOG_WARNING, "CANVAS: could not load %s", path.c_str());
    }
  }

  // Escape belongs to whatever is being typed into first: cancelling an edit is
  // not a request for the canvas menu.
  if (IsKeyPressed(KEY_ESCAPE) && chats_.Editing()) {
    // Out of the conversation, the way Escape is out of anything being typed.
    chats_.Blur();
  } else if (IsKeyPressed(KEY_ESCAPE) && !media_.Editing()) {
    menu_.Toggle();
    chats_.Blur();
    options_.apiKeyNote.clear();
    // Opening the menu ends editing, so no caret is left blinking behind it.
    if (menu_.Visible()) SetFocus(nullptr);
  }

  // A press that the menu acted on belongs to the menu, even when acting on it
  // was what closed the menu. Otherwise choosing a recent canvas would put the
  // caret into whatever box of the *new* canvas happened to be under the
  // pointer -- the click would be spent twice, on two different canvases.
  const bool menuOwnsPress = menu_.Visible();

  switch (menu_.Update(&options_)) {
    case MenuAction::NewCanvas:
      NewCanvas();
      menu_.Close();
      break;
    case MenuAction::Fullscreen:
      // The menu goes, since asking for the whole screen is asking to look at
      // the canvas.
      SetFullscreen(!options_.fullscreen);
      menu_.Close();
      break;
    case MenuAction::AlwaysCode:
      // The menu stays, so the tick can be seen to have changed.
      SetAlwaysCode(!options_.alwaysCode);
      break;
    case MenuAction::PasteApiKey: {
      // Whatever is on the clipboard, if it is a key. The menu stays, so the
      // button can say which key is now set.
      const char* clip = GetClipboardText();
      std::string key = clip == nullptr ? std::string() : std::string(clip);
      while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
      std::size_t lead = 0;
      while (lead < key.size() && std::isspace(static_cast<unsigned char>(key[lead]))) ++lead;
      key.erase(0, lead);
      if (key.rfind("sk-ant-", 0) == 0 && key.find_first_of(" \t\r\n") == std::string::npos) {
        settings::RememberApiKey(key);
        options_.apiKeyTail = KeyTail(settings::ApiKey());
        options_.apiKeyNote = options_.apiKeyTail.empty() ? "Could not keep the key" : "";
      } else {
        options_.apiKeyNote = "No API key on the clipboard";
      }
      break;
    }
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
    case MenuAction::AskClaude: {
      SetFocus(nullptr);
      const Vector2 size = ChatPanel::DefaultSize();
      const Vector2 at = FindGap(size, WindowRects());
      chats_.New({at.x + size.x * 0.5f, at.y + size.y * 0.5f}, viewport_.Zoom());
      CentreOn({at.x, at.y, size.x, size.y});
      menu_.Close();
      break;
    }
    case MenuAction::NewSlide: {
      slides_.AddSlide(options_.slideAspect, &viewport_);
      menu_.Close();
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
        // Into the nearest gap to the middle of the view. Its size is only known
        // once it has loaded, so it is put down and then moved there.
        const std::vector<Rectangle> taken = WindowRects();
        const Vector2 middle = viewport_.ToWorld({static_cast<float>(GetScreenWidth()) * 0.5f,
                                                  static_cast<float>(GetScreenHeight()) * 0.5f});
        if (media_.Add(path, sb::SnapToGrid(middle, viewport_.Zoom()))) {
          const int       index = static_cast<int>(media_.Items().size()) - 1;
          const Rectangle bounds = media_.WorldBounds(index);
          const Vector2   at = FindGap({bounds.width, bounds.height}, taken);
          media_.Items().back()->worldPos = at;
          CentreOn({at.x, at.y, bounds.width, bounds.height});
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
  if (canvasActive && !consumed) consumed = HandlePull();
  if (canvasActive && textDrag_ != TextDrag::None) consumed = HandleTextGesture();
  if (canvasActive && !consumed && dragBlock_ != nullptr) consumed = HandleBlockDrag();
  if (canvasActive && !consumed && media_.Busy()) consumed = media_.HandleInput(viewport_);
  if (canvasActive && !consumed && axes_.Dragging()) {
    consumed = axes_.HandleInput(viewport_, fonts_, codeBlocks_);
  }
  if (canvasActive && !consumed && plots_.Dragging()) {
    consumed = plots_.HandleInput(viewport_, fonts_);
  }
  if (canvasActive && !consumed && (surfaces_.Dragging() || surfaces_.Orbiting())) {
    consumed = surfaces_.HandleInput(viewport_, fonts_);
  }
  if (canvasActive && !consumed && (volumes_.Dragging() || volumes_.Orbiting())) {
    consumed = volumes_.HandleInput(viewport_, fonts_);
  }
  if (canvasActive && !consumed && shaders_.Dragging()) {
    consumed = shaders_.HandleInput(viewport_, fonts_);
  }
  if (canvasActive && !consumed && colours_.Dragging()) {
    consumed = colours_.HandleInput(viewport_, fonts_);
  }
  // An orbit is a drag too, and owns the pointer the same way.
  if (canvasActive && !consumed && (spaces_.Dragging() || spaces_.Orbiting())) {
    consumed = spaces_.HandleInput(viewport_, fonts_);
  }

  // A conversation is drawn over everything else, so it is asked first -- its
  // own gestures, and the wheel, which scrolls it rather than resizing whatever
  // is underneath. Taking the caret into one takes it out of any box.
  if (canvasActive && !consumed) {
    consumed = chats_.HandleInput(viewport_, fonts_);
    if (chats_.Editing() && focus_ != nullptr) SetFocus(nullptr);
  }

  // The wheel resizes whatever it is over. It is asked early, before the rims
  // start claiming the pointer, because a wheel event is never ambiguous: no
  // drag can be in flight while one arrives.
  if (canvasActive && !consumed) consumed = HandleResize();

  // Otherwise input goes to the innermost thing first: a box's buttons -- the
  // ones on a focused code box, and the ones a text box shows whenever you
  // reach for them -- then the windows, then the canvas.
  if (canvasActive && !consumed) consumed = colours_.HandleMenus(viewport_, fonts_);
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
  if (canvasActive && !consumed) consumed = colours_.HandleInput(viewport_, fonts_);
  if (canvasActive && !consumed) consumed = surfaces_.HandleInput(viewport_, fonts_);
  if (canvasActive && !consumed) consumed = volumes_.HandleInput(viewport_, fonts_);
  if (canvasActive && !consumed) consumed = spaces_.HandleInput(viewport_, fonts_);
  if (canvasActive && !consumed) {
    consumed = slides_.HandleInput(viewport_, fonts_, options_.slideAspect, &viewport_);
  }

  if (canvasActive && !consumed) {
    // Starting one: a press on a selected window's rim moves the whole
    // selection, and a press on the background begins a loop.
    HandleSelection();
  } else if (!canvasActive) {
    // Only the menu opening abandons a gesture. Something else taking the
    // pointer does not: that is what the in-flight pass above is for.
    lassoActive_ = false;
    textDrag_ = TextDrag::None;
    textBlock_ = nullptr;
    pull_ = Pull{};
  }

  // Keys that act on the selection, and only while no box is being typed into.
  // Not while a selection is being carried: the drag is holding raw pointers to
  // the very boxes these would destroy.
  // Nor while a viz is being pulled out: Ctrl is already down, so Ctrl+X is a
  // keystroke away from freeing the box the pull is pointing into.
  const bool keysActOnSelection = canvasActive && focus_ == nullptr && !chats_.Editing() &&
                                  !selectionDrag_ && pull_.from == PullFrom::None;
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
  if (canvasActive && focus_ == nullptr && !chats_.Editing() && !ctrl) {
    if (IsKeyPressed(KEY_RIGHT)) slides_.Step(+1, &viewport_, options_.slideAspect);
    if (IsKeyPressed(KEY_LEFT)) slides_.Step(-1, &viewport_, options_.slideAspect);
  }

  // Home frames what you are looking at. Not a character, so it cannot start a
  // box the way a letter typed onto the canvas does; and not while a caret is
  // anywhere, where Home is the start of the line.
  if (canvasActive && focus_ == nullptr && !media_.Editing() && !chats_.Editing() &&
      IsKeyPressed(KEY_HOME)) {
    FrameOnScreen();
  }

  // A slide that has just appeared gets its title and its body, as two windows
  // of the canvas rather than as text the slide owns.
  if (const NewSlide fresh = slides_.TakeNewSlide(); fresh.made) {
    FurnishSlide(fresh.centre, "Title", {"Lorem ipsum", "dolor sit amet"});
  }
  if (const SlideRemoval gone = slides_.TakeRemoval(); gone.asked) {
    ThrowAwaySlide(gone.bounds, gone.shift);
  }
  media_.Advance(GetFrameTime());
  chats_.Update();
  ServeTools();
  ConverseNext();
  if (chats_.TakeChanged()) ScheduleAutosave();
  if (std::string canvas; chats_.TakeCanvasRewind(&canvas)) RestoreCanvas(canvas);
  // What each function being read is fed with, before the boxes are polled --
  // polling is where a box puts its spread through its function.
  FeedFunctions();
  ShareAcrossCanvas();
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->Poll();

  // A box that has just gained (or lost) number lines is a different size than
  // it was a frame ago, so this is the moment to check nobody is sitting on
  // anybody. Only on the change: it is far too much work to do every frame.
  // What the canvas says about how things are drawn. Rebuilt every frame: a
  // viz is not registered, it is written down somewhere and therefore true, and
  // the boxes saying so are edited like any others. Nothing else has to be
  // rebuilt when it changes -- a viz box owns its own shaders, and a window
  // asking for one simply gets the new one.
  vizes_.Sync(codeBlocks_, canvas_.signature);

  bool shapesChanged = axes_.Sync(codeBlocks_);
  shapesChanged = plots_.Sync(codeBlocks_) || shapesChanged;
  shapesChanged = shaders_.Sync(codeBlocks_) || shapesChanged;
  shapesChanged = colours_.Sync(codeBlocks_) || shapesChanged;
  shapesChanged = surfaces_.Sync(codeBlocks_) || shapesChanged;
  shapesChanged = volumes_.Sync(codeBlocks_) || shapesChanged;
  shapesChanged = spaces_.Sync(codeBlocks_, vizes_) || shapesChanged;
  if (shapesChanged) SettleBlocks();

  // **The pointer and the keyboard are two different claims.** Everything above
  // is about the pointer, and a window that has taken the pointer has not taken
  // the keys: a box with the caret in it keeps them wherever the mouse happens
  // to be sitting. Typing used to stand down for whatever the pointer was over,
  // which is a rule nobody can see the edge of -- a box shrinking under a held
  // backspace slides a number line up under a motionless pointer, and the
  // typing stops in the middle of a word for no reason the typist can point at.
  //
  // The one thing that does take the keys is a video's trim time being typed
  // into, because that is a caret too, and there cannot be two.
  if (canvasActive && chats_.Editing()) {
    chats_.HandleKeys(fonts_);
  } else if (canvasActive && !lassoActive_ && !selectionDrag_ && !media_.Editing()) {
    HandleTyping();
  }

  // A window being hovered or dragged lights up the variable it stands for,
  // wherever that variable is written. Recomputed every frame: it is a property
  // of where the pointer is, not something to remember.
  // Which sample the pointer is on, at either end. Cleared every frame and set
  // again: it is a property of where the pointer is, not something to remember.
  // Whether a gesture is in flight, which is what turns hovering off (hover.h).
  // Every gesture is already listed in the pass above that hands it the
  // pointer; this is the same list, asked once for all of them. A finger down
  // counts, since touch has no hover to lose and a pan or a pinch sweeps across
  // everything on the way.
  bool held = lassoActive_ || selectionDrag_ || compose_ != Compose::None ||
              pull_.from != PullFrom::None || fingerCarry_ ||
              textDrag_ != TextDrag::None || dragBlock_ != nullptr ||
              (media_.Busy() && !media_.Editing()) || chats_.Busy() || axes_.Dragging() ||
              plots_.Dragging() ||
              shaders_.Dragging() || colours_.Dragging() || surfaces_.Dragging() ||
              surfaces_.Orbiting() ||
              volumes_.Dragging() || volumes_.Orbiting() || spaces_.Dragging() ||
              spaces_.Orbiting() || viewport_.PanningWithMouse() || touch::Count() > 0;
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    held = held || block->SettingMeasure();
  }
  // And while the menu is up, the canvas behind it is not being pointed at
  // either: the pointer is choosing from the menu, and a box flipping to code
  // under the scrim is the canvas answering a question nobody asked.
  held = held || menu_.Visible();
  hover::SetHeld(held);
  hover::SetCovered(!held && canvasActive && chats_.ItemAt(GetMousePosition(), viewport_) >= 0);
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    block->SetPlayPaused(focus_ != nullptr);
  }
  TrackUndo(held);
  TrackAutosave(held);

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->SetSampleHover(-1);
  if (canvasActive) {
    const CodeBlock* onBlock = nullptr;
    int              onSample = -1;
    const Vector2    pointer = hover::Pointer();
    if (axes_.SampleUnder(pointer, viewport_, fonts_, &onBlock, &onSample) ||
        spaces_.SampleUnder(pointer, viewport_, fonts_, &onBlock, &onSample)) {
      if (onBlock != nullptr) onBlock->SetSampleHover(onSample);
    }
  }

  std::vector<ActiveValue> active;
  if (canvasActive) {
    axes_.ActiveVariables(viewport_, fonts_, &active);
    plots_.ActiveVariables(viewport_, fonts_, &active);
    shaders_.ActiveVariables(viewport_, fonts_, &active);
    colours_.ActiveVariables(viewport_, fonts_, &active);
    spaces_.ActiveVariables(viewport_, fonts_, &active);
  }

  // Taking hold of a name in the text is the same statement as taking hold of
  // its window, so it lights up the same things: the name wherever else it is
  // written, the wire, and the viz blinking at the other end. The reverse of
  // pulling a variable out of the code, and the same gesture -- highlight a
  // mention -- read from the other side.
  const auto lightUp = [&](CodeBlock* block, const std::string& name) {
    if (name.empty()) return;
    // A name shared from another box is lit from its home, where its windows
    // are; the step below then lights it wherever else it is used.
    block = HomeOf(block, name);
    for (const ActiveValue& value : active) {
      if (value.block == block && value.name == name) return;
    }
    Vector2 at{};
    // Every window that stands for a value is asked, so that pointing at a name
    // is two-way for every kind of value rather than only the ones asked first.
    if (axes_.MarkerFor(block, name, viewport_, fonts_, &at) ||
        plots_.MarkerFor(block, name, viewport_, fonts_, &at) ||
        colours_.MarkerFor(block, name, viewport_, fonts_, &at) ||
        spaces_.MarkerFor(block, name, viewport_, fonts_, &at)) {
      active.push_back({block, name, at});
    }
  };
  if (canvasActive && focus_ != nullptr) {
    for (const std::string& name : focus_->HighlightedVariables()) lightUp(focus_, name);
  }
  if (canvasActive && pull_.from != PullFrom::None) lightUp(pull_.block, pull_.name);
  // **And pointing at a mention is pointing at the thing.** Hovering a window
  // boxes its name and wires the two together, so hovering the name does the
  // same from the other end -- the same set, so the two cannot come to disagree
  // about what "this variable" lights up. Through the hover pointer, like every
  // other hover, so a gesture sweeping across a box leaves its names alone.
  if (canvasActive) {
    const Vector2 aim = hover::Pointer();
    if (CodeBlock* over = BlockAt(aim); over != nullptr) {
      lightUp(over, over->MentionAt(aim, viewport_, fonts_));
    }
  }
  // Pointing at the bare part of a number line is an offer to put a value into
  // the box that axis belongs to. The box answers by blinking a caret where the
  // declaration would land, so the click has somewhere visible to be aimed.
  const CodeBlock* insertInto = canvasActive ? axes_.InsertTarget(viewport_, fonts_) : nullptr;

  // **A shared thing is one thing wherever it is used.** Whatever is lit in its
  // home box is lit in every box that uses it too, wired from the same window --
  // so the mentions in another box gather into the same bundle.
  {
    const std::size_t lit = active.size();
    for (std::size_t i = 0; i < lit; ++i) {
      const ActiveValue value = active[i];
      for (const std::unique_ptr<CodeBlock>& other : codeBlocks_) {
        if (other.get() == value.block) continue;
        const std::vector<std::string>& taken = other->Program().importedNames();
        if (std::find(taken.begin(), taken.end(), value.name) == taken.end()) continue;
        if (HomeOf(other.get(), value.name) != value.block) continue;
        ActiveValue there = value;
        there.block = other.get();
        active.push_back(there);
      }
    }
  }

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
  // A two-finger swipe turns a page, which is the same thing left and right
  // do -- so it is offered on the same terms: only from a slide, since
  // elsewhere the deck is not what the gesture would be about, and elsewhere
  // two fingers are still the pan and the pinch.
  const bool onSlide = slides_.SlideAtCamera(viewport_, options_.slideAspect) >= 0;
  viewport_.Update(canvasActive, !consumed,
                   !axes_.Scrolling() && !spaces_.Orbiting() && !surfaces_.Orbiting() &&
                       !volumes_.Orbiting() && !colours_.Orbiting(),
                   onSlide);
  if (const int swipe = viewport_.TakeSwipe(); swipe != 0) {
    slides_.Step(swipe, &viewport_, options_.slideAspect);
  }

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
                        plots_.Carrying() || shaders_.Carrying() || colours_.Carrying() ||
                        surfaces_.Carrying() ||
                        volumes_.Carrying() || spaces_.Carrying() ||
                        media_.Carrying() || chats_.Carrying();
  // The wheel over a conversation scrolls it, which places nothing.
  const bool scrolling = chats_.ItemAt(GetMousePosition(), viewport_) >= 0;
  const bool scaling =
      (GetMouseWheelMoveV().y != 0.0f && !scrolling) || touch::Count() >= 2;
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
  // Everything the rest of this function asks for is off limits to the atlas
  // evictor until the next frame, so nothing can drop a texture another window
  // is still drawing from.
  fonts_.BeginFrame();

  // A look Claude asked for is drawn first, unseen, and read back; the frame
  // proper is then drawn over it.
  TakeLooks();
  DrawCanvas(/*look=*/false);

  // Selected windows wear their rim whether or not they are being pointed at.
  axes_.DrawSelectionRims(viewport_, fonts_);
  plots_.DrawSelectionRims(viewport_, fonts_);
  shaders_.DrawSelectionRims(viewport_, fonts_);
  colours_.DrawSelectionRims(viewport_, fonts_);
  colours_.DrawChrome(viewport_, fonts_);
  surfaces_.DrawSelectionRims(viewport_, fonts_);
  volumes_.DrawSelectionRims(viewport_, fonts_);
  spaces_.DrawSelectionRims(viewport_, fonts_);
  media_.DrawSelectionRims(viewport_);
  chats_.DrawSelectionRims(viewport_);

  // The name of the window being pointed at, over every window so none covers it.
  windowname::Draw(fonts_);

  if (lassoActive_) DrawLasso(lasso_);

  // Carrying a window brings up the one place it can be thrown away.
  if (dragBlockMoved_ || selectionMoved_ || axes_.Carrying() || plots_.Carrying() ||
      shaders_.Carrying() || colours_.Carrying() || surfaces_.Carrying() ||
      volumes_.Carrying() ||
      spaces_.Carrying() || media_.Carrying() || chats_.Carrying()) {
    DrawDropTarget(OverDropTarget(carry::Position()));
  }

  menu_.Draw(fonts_, options_);

  // Over everything, the menu included: it is the pointer.
  if (cursorHidden_) DrawGrabCursor(GetMousePosition());
}

void App::DrawCanvas(bool look) {
  ClearBackground(kCanvasBg);
  if (!look) DrawDotGrid(viewport_, gridShow_);
  // Under everything else: a slide is a region of the canvas, and whatever is
  // on it stands on top of it.
  slides_.Draw(viewport_, fonts_, options_.slideAspect);

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->Draw(viewport_, fonts_);
  for (const MathBlock& block : mathBlocks_) block.Draw(viewport_, fonts_);

  axes_.Draw(viewport_, fonts_, vizes_);
  plots_.Draw(viewport_, fonts_);
  shaders_.Draw(viewport_, fonts_);
  colours_.Draw(viewport_, fonts_, vizes_);
  surfaces_.Draw(viewport_, fonts_);
  volumes_.Draw(viewport_, fonts_);
  spaces_.Draw(viewport_, fonts_, vizes_);
  media_.Draw(viewport_, fonts_);

  // A look is of the canvas itself: no controls, no conversations, and no wires
  // drawn from where the pointer was on the screen everyone else is seeing.
  if (look) {
    for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
      block->DrawOverlay(viewport_, fonts_);
    }
    return;
  }

  // Every box's controls, after every box: a strip half under the card next to
  // it is a strip you cannot press.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    block->DrawChrome(viewport_, fonts_);
  }
  chats_.Draw(viewport_, fonts_);

  // Two values held together, and the choice of what to make of them.
  DrawCompose();

  // Where a value came from, over everything: a wiring diagram chopped in half
  // by a card would say nothing.
  for (const ActiveValue& value : active_) {
    if (value.wired) DrawWiring(value);
  }

  // And the one sample the pointer is on, joined to what it turned into.
  DrawSampleLink();

  // Error balloons last, so they are never buried under a neighbouring card.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    block->DrawOverlay(viewport_, fonts_);
  }
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

bool App::WriteCanvasFile(const std::string& path) {
  // Beside it first, then over it: a save interrupted halfway leaves the old
  // file whole rather than a new one in pieces.
  const std::string temporary = path + ".saving";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    WriteCanvas(file, path, /*withView=*/true);
    if (!file.good()) return false;
  }
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
    return false;
  }
  savedSnapshot_ = Snapshot();
  return true;
}

bool App::SaveCanvas(const std::string& path) {
  const std::string previous = currentPath_;
  const bool        wasUntitled = untitled_;
  if (!WriteCanvasFile(path)) return false;
  currentPath_ = path;
  SetCanvasFolder(std::filesystem::path(path).parent_path().string());
  untitled_ = false;
  autosaveDue_ = 0.0;
  recent::Remember(path);
  // It has a real name now, so the file it was being kept in until it had one
  // is not a canvas any more, only a stale copy of this one.
  if (wasUntitled && !previous.empty() && previous != path) {
    std::error_code ec;
    std::filesystem::remove(previous, ec);
  }
  return true;
}

void App::WriteCanvas(std::ostream& file, const std::string& path, bool withView,
                      bool withChats) {
  file << "sketchbook " << kFileVersion << "\n";
  if (withView) {
    const Vector2 centre = viewport_.Centre();
    file << "view " << centre.x << " " << centre.y << " " << viewport_.Zoom() << "\n";
  }
  file << "aspect " << options_.slideAspect << "\n";
  // Only written when ticked, so a canvas that never asked for it reads as before.
  if (options_.alwaysCode) file << "alwayscode 1\n";
  // A slide is only its rim now: its title and its body are text boxes, and so
  // they are saved as boxes along with every other window.
  for (const Slide& slide : slides_.Slides()) {
    file << "slide " << slide.centre.x << " " << slide.centre.y << "\n";
  }

  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    const Vector2 position = block->WorldPosition();
    file << "block " << position.x << " " << position.y << " " << block->Em() << " "
         << block->ReadingMode() << " " << (block->Bold() ? 1 : 0) << " "
         << (block->Italic() ? 1 : 0) << " " << static_cast<int>(block->Justification())
         << " " << block->TextWidth() << " " << (block->ShowSource() ? 1 : 0) << "\n";
    for (const std::string& line : block->Lines()) file << "> " << line << "\n";
    // A knob's number, where the text does not already say it: a real under its
    // name, a colour as its three parts. A free function's choice lives in its
    // example generator, which is deterministic in the variable's name.
    for (const auto& [name, value] : block->KnobValues()) {
      file << "val " << name << " " << value << "\n";
    }
    for (const std::string& name : block->Hidden()) file << "hide " << name << "\n";
    if (block->DrawnHidden(CodeBlock::kPicture)) file << "hideshader\n";
    if (block->DrawnHidden(CodeBlock::kSurface)) file << "hidesurface\n";
    if (block->DrawnHidden(CodeBlock::kVolume)) file << "hidevolume\n";
    if (block->DrawnHidden(CodeBlock::kColour)) file << "hidecolour\n";
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
    // A field is on a row the same way a value is, and a row two of them were
    // merged onto has to come back merged.
    for (const AxisRef& ref : line.fields) {
      file << "field " << indexOf(ref.block) << " " << ref.name << "\n";
    }
  }

  // Which way it is being looked at, and from where, is part of what was set up.
  for (const ColorView& view : colours_.Views()) {
    file << "colour " << (view.docked ? 1 : 0) << " " << view.worldPos.x << " "
         << view.worldPos.y << " " << indexOf(view.block) << " "
         << static_cast<int>(view.shape) << " " << view.yaw << " " << view.pitch << "\n";
  }
  for (const ShaderView& view : shaders_.Views()) {
    file << "shader " << (view.docked ? 1 : 0) << " " << view.worldPos.x << " "
         << view.worldPos.y << " " << indexOf(view.block) << " " << view.centre.x << " "
         << view.centre.y << " " << view.span << "\n";
  }

  // Where you were looking from is part of what you set up, so the orbit goes
  // with the window -- for a surface and a volume alike.
  for (const SurfaceView& view : surfaces_.Views()) {
    file << "surface " << (view.docked ? 1 : 0) << " " << view.worldPos.x << " "
         << view.worldPos.y << " " << indexOf(view.block) << " " << view.centre.x << " "
         << view.centre.y << " " << view.span << " " << view.orbit.x << " " << view.orbit.y
         << " " << view.orbit.z << "\n";
  }
  for (const VolumeView& view : volumes_.Views()) {
    file << "volume " << (view.docked ? 1 : 0) << " " << view.worldPos.x << " "
         << view.worldPos.y << " " << indexOf(view.block) << " " << view.centre.x << " "
         << view.centre.y << " " << view.span << " " << view.orbit.x << " " << view.orbit.y
         << " " << view.orbit.z << "\n";
  }

  // Where you were looking from is part of what you set up, so the orbit is
  // saved with the window.
  for (const SpaceWindow& window : spaces_.Windows()) {
    file << "space " << (window.kind == SpaceKind::Space ? 0 : 1) << " "
         << (window.docked ? 1 : 0) << " " << window.worldPos.x << " " << window.worldPos.y
         << " " << indexOf(window.dock) << " " << window.yaw << " " << window.pitch << " "
         << window.dist << " " << (window.cullOutside ? 1 : 0) << " " << (window.home ? 1 : 0)
         << "\n";
    // Which points it holds, since a point can have been pulled out of its box's
    // window or merged into another box's.
    for (const PointRef& ref : window.refs) {
      file << "point " << indexOf(ref.block) << " " << ref.name << "\n";
    }
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

  // Conversations, one a line, in the file but not in an undo step: a long one
  // would make every step large, and an undo is about the canvas, not about
  // what was said.
  if (withView) {
    for (const json::Value& chat : chats_.Save()) file << "chat " << json::Write(chat) << "\n";
  } else if (withChats) {
    // Which conversations are open, and where: what an undo step brings back.
    file << "chats\n";
    for (const ChatPanel::Ref& ref : chats_.Refs()) {
      file << "chatref " << ref.id << " " << ref.worldPos.x << " " << ref.worldPos.y << " "
           << ref.width << " " << ref.height << " " << ref.madeBelow << "\n";
    }
  }
}

bool App::LoadCanvas(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  // Whatever was done to the canvas being left is written down before it goes.
  FlushAutosave();
  if (!ReadCanvas(file, path, /*restoring=*/false)) return false;
  currentPath_ = path;
  SetCanvasFolder(std::filesystem::path(path).parent_path().string());
  untitled_ = false;
  autosaveDue_ = 0.0;
  recent::Remember(path);
  // A different canvas: nothing done to the last one can be undone into this.
  ResetUndo();
  // And it is what is on disk, so opening it is not a change to save.
  savedSnapshot_ = undoBase_;
  return true;
}

bool App::ReadCanvas(std::istream& file, const std::string& path, bool restoring) {
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
    // 0 auto, 1 mathematics, 2 code. A canvas written before there was a third
    // state says 0 or 1, which mean the same two things they always did.
    int                                       showMath = 0;
    bool                                      bold = false;
    bool                                      italic = false;
    int                                       justify = 0;
    float                                     textWidth = 0.0f;
    bool                                      showSource = false;
    std::vector<std::string>                  lines;
    std::vector<std::pair<std::string, float>> values;
    std::vector<std::string>                   hidden;
    // One per kind of window a box can draw into, so closing a picture does not
    // take the surface next to it.
    bool                                       shaderHidden = false;
    bool                                       surfaceHidden = false;
    bool                                       volumeHidden = false;
    bool                                       colourHidden = false;
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
    Vector2 centre{};
    float   span = 4.0f;
  };
  // A surface and a volume are read the same way and differ only in what they
  // default to, so one shape carries both.
  struct LoadedTurned {
    bool    docked = true;
    Vector2 worldPos{};
    int     block = -1;
    Vector2 centre{};
    float   span = 4.0f;
    Vector3 orbit{};
  };
  struct LoadedSpace {
    int     kind = 0;
    bool    docked = true;
    Vector2 worldPos{};
    int     block = -1;
    float   yaw = 1.09f;
    float   pitch = -0.25f;
    float   dist = 11.0f;
    bool    cull = true;
    bool    home = true;
    std::vector<std::pair<int, std::string>> points;
  };
  struct LoadedAxis {
    bool                                     docked = true;
    Vector2                                  worldPos{};
    float                                    centre = 0.0f;
    float                                    span = 4.0f;
    int                                      dock = -1;
    std::vector<std::pair<int, std::string>> refs;
    std::vector<std::pair<int, std::string>> fields;
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
  std::vector<std::pair<int, ColorView>> colourViews;
  std::vector<LoadedTurned> surfaceViews;
  std::vector<LoadedTurned> volumeViews;
  std::vector<LoadedSpace>  spaceWindows;
  std::vector<LoadedMedia>  media;
  std::vector<LegacySlideText> legacy;
  Vector2 centre{0.0f, 0.0f};
  float   zoom = 1.0f;
  bool    inBlock = false;
  float   aspect = kDefaultSlideAspect;
  int     alwaysCode = 0;
  std::vector<Slide> slides;

  std::vector<json::Value> chats;
  bool                     haveChatRefs = false;
  std::vector<ChatPanel::Ref> chatRefs;
  std::string raw;
  while (std::getline(file, raw)) {
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    if (raw.empty()) continue;

    if (raw == "chats") {
      haveChatRefs = true;
      continue;
    }
    if (raw.rfind("chatref ", 0) == 0) {
      std::istringstream ref(raw.substr(8));
      ChatPanel::Ref one;
      ref >> one.id >> one.worldPos.x >> one.worldPos.y >> one.width >> one.height >> one.madeBelow;
      chatRefs.push_back(one);
      continue;
    }
    // A conversation is one JSON object taking the rest of the line.
    if (raw.rfind("chat ", 0) == 0) {
      json::Value chat;
      if (json::Parse(std::string_view(raw).substr(5), &chat)) chats.push_back(std::move(chat));
      continue;
    }
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
      block.showMath = showMath;
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
    } else if (keyword == "alwayscode") {
      stream >> alwaysCode;
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
    } else if (keyword == "hidesurface" && inBlock) {
      blocks.back().surfaceHidden = true;
    } else if (keyword == "hidevolume" && inBlock) {
      blocks.back().volumeHidden = true;
    } else if (keyword == "hidecolour" && inBlock) {
      blocks.back().colourHidden = true;
    } else if (keyword == "colour") {
      int docked = 1;
      int shape = 0;
      int block = -1;
      ColorView view;
      stream >> docked >> view.worldPos.x >> view.worldPos.y >> block >> shape >> view.yaw >>
          view.pitch;
      view.docked = (docked != 0);
      view.shape = static_cast<ColorShape>(std::clamp(shape, 0, 2));
      colourViews.emplace_back(block, view);
    } else if (keyword == "shader") {
      LoadedShader view;
      int docked = 1;
      stream >> docked >> view.worldPos.x >> view.worldPos.y >> view.block;
      // The patch of the plane on show, absent from canvases written before a
      // picture had one. Whatever is missing keeps its default.
      stream >> view.centre.x >> view.centre.y >> view.span;
      view.docked = (docked != 0);
      shaderViews.push_back(view);
    } else if (keyword == "surface" || keyword == "volume") {
      LoadedTurned view;
      int          docked = 1;
      stream >> docked >> view.worldPos.x >> view.worldPos.y >> view.block >> view.centre.x >>
          view.centre.y >> view.span >> view.orbit.x >> view.orbit.y >> view.orbit.z;
      view.docked = (docked != 0);
      (keyword == "surface" ? surfaceViews : volumeViews).push_back(view);
    } else if (keyword == "space") {
      LoadedSpace window;
      int docked = 1;
      int cull = 1;
      stream >> window.kind >> docked >> window.worldPos.x >> window.worldPos.y >>
          window.block >> window.yaw >> window.pitch >> window.dist >> cull;
      window.docked = (docked != 0);
      window.cull = (cull != 0);
      // A canvas written before windows could be split has no flag, and every
      // window in it was its box's own.
      int home = 1;
      if (!(stream >> home)) home = 1;
      window.home = (home != 0);
      spaceWindows.push_back(window);
    } else if (keyword == "point" && !spaceWindows.empty()) {
      int         index = -1;
      std::string name;
      stream >> index >> name;
      spaceWindows.back().points.emplace_back(index, name);
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
    } else if (keyword == "field" && !axes.empty()) {
      int index = -1;
      std::string name;
      stream >> index >> name;
      axes.back().fields.emplace_back(index, name);
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
  colours_.Clear();
  surfaces_.Clear();
  volumes_.Clear();
  spaces_.Clear();
  // Every picture's texture and every video's decoder, by hand and while the
  // graphics context is still here.
  media_.Clear();
  // Conversations are not in the file yet, so an undo step leaves them be; a
  // different canvas does not, and its replies in flight are stopped.
  if (!restoring) {
    chats_.Clear();
    toolJobs_.clear();
    chats_.Adopt(chats);
  } else if (haveChatRefs) {
    // An undo step: the conversations it had, as they were placed. One thrown
    // away comes back with all it said.
    chats_.Restore(chatRefs);
  }
  // Restoring an undo step keeps every box whose text is exactly what the step
  // says, so only the boxes that were actually edited are built again. Whatever
  // is left over afterwards is thrown away, by hand.
  std::vector<std::unique_ptr<CodeBlock>> previous = std::move(codeBlocks_);
  codeBlocks_.clear();
  mathBlocks_.clear();
  if (!restoring) {
    for (const std::unique_ptr<CodeBlock>& block : previous) block->ReleaseGpu();
    previous.clear();
  }

  for (LoadedBlock& loaded : blocks) {
    std::unique_ptr<CodeBlock> block;
    for (std::unique_ptr<CodeBlock>& candidate : previous) {
      if (candidate != nullptr && candidate->Lines() == loaded.lines) {
        block = std::move(candidate);
        break;
      }
    }
    // Undoing a knob drag changes a number in the text, and a box is matched by
    // its text -- so it would be built again from nothing. The same box in the
    // same place with the same number of lines is taken to be that box, given
    // its old lines back, and rebuilt; if only numbers differ, that rebuild is
    // the shader it already has and nothing is compiled.
    bool relined = false;
    if (block == nullptr && restoring) {
      for (std::unique_ptr<CodeBlock>& candidate : previous) {
        if (candidate == nullptr || candidate->Lines().size() != loaded.lines.size()) continue;
        const Vector2 at = candidate->WorldPosition();
        if (std::abs(at.x - loaded.position.x) > 0.01f || std::abs(at.y - loaded.position.y) > 0.01f) {
          continue;
        }
        block = std::move(candidate);
        relined = true;
        break;
      }
    }
    const bool kept = (block != nullptr);
    if (relined) block->RestoreLines(loaded.lines);
    if (kept) {
      block->SetWorldPosition(loaded.position);
      block->SetEm(loaded.em);
    } else {
      block = std::make_unique<CodeBlock>(loaded.position, loaded.em, std::move(loaded.lines));
    }
    block->SetReadingMode(loaded.showMath);
    block->SetBold(loaded.bold);
    block->SetItalic(loaded.italic);
    block->SetJustification(static_cast<CodeBlock::Justify>(loaded.justify));
    block->SetTextWidth(loaded.textWidth);
    block->SetShowSource(loaded.showSource);
    block->RestoreHidden(std::move(loaded.hidden));
    block->SetDrawnHidden(CodeBlock::kPicture, loaded.shaderHidden);
    block->SetDrawnHidden(CodeBlock::kSurface, loaded.surfaceHidden);
    block->SetDrawnHidden(CodeBlock::kVolume, loaded.volumeHidden);
    block->SetDrawnHidden(CodeBlock::kColour, loaded.colourHidden);
    if (kept) {
      // Already built, so its knobs are set where they are rather than on the
      // next build -- there is not going to be one. And whatever was swinging
      // stops: a swing carrying on would retype the very number just undone.
      for (const CodeBlock::Play& play : std::vector<CodeBlock::Play>(block->Plays())) {
        block->StopPlaying(play.name);
      }
      for (const auto& [name, value] : loaded.values) {
        const mathprogram::Variable* variable = block->FindVariable(name);
        if (variable != nullptr && variable->free && !variable->written &&
            variable->kind == mathprogram::Kind::Real && variable->value != value) {
          block->SetVariableValue(name, value, /*intoText=*/false);
        }
      }
      // A colour comes back as its three parts.
      for (const mathprogram::Variable& variable : block->Variables()) {
        if (variable.kind != mathprogram::Kind::Color || !variable.free || variable.written ||
            variable.literals.empty() || variable.literals.front().components.size() != 3) {
          continue;
        }
        float rgb[3] = {variable.literals.front().components[0],
                        variable.literals.front().components[1],
                        variable.literals.front().components[2]};
        const char* const parts[] = {"r", "g", "b"};
        bool changed = false;
        for (int i = 0; i < 3; ++i) {
          for (const auto& [name, value] : loaded.values) {
            if (name == variable.name + "." + parts[i] && rgb[i] != value) {
              rgb[i] = value;
              changed = true;
            }
          }
        }
        if (changed) block->SetColorValue(variable.name, rgb, /*intoText=*/false);
      }
      // And a multivector as one number per member.
      for (const mathprogram::Variable& variable : block->Variables()) {
        if (variable.kind != mathprogram::Kind::Multivector || !variable.free ||
            variable.literals.empty() || !variable.literals.front().array) {
          continue;
        }
        std::vector<float> parts = variable.literals.front().components;
        const std::vector<std::string> members =
            clifford::MemberNames(variable.type, block->Algebra());
        bool changed = false;
        for (std::size_t i = 0; i < parts.size() && i < members.size(); ++i) {
          for (const auto& [name, value] : loaded.values) {
            if (name == variable.name + "." + members[i] && parts[i] != value) {
              parts[i] = value;
              changed = true;
            }
          }
        }
        if (changed) block->SetComponentValues(variable.name, parts);
      }
    } else {
      block->RestoreValues(std::move(loaded.values));
      block->RequestCompile();
    }
    codeBlocks_.push_back(std::move(block));
  }
  for (const std::unique_ptr<CodeBlock>& gone : previous) {
    if (gone != nullptr) gone->ReleaseGpu();
  }
  previous.clear();

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
    for (const auto& [index, name] : loaded.fields) {
      if (index < 0 || index >= static_cast<int>(codeBlocks_.size())) continue;
      line.fields.push_back({codeBlocks_[static_cast<std::size_t>(index)].get(), name});
    }
    // A row framed by hand keeps its scale; a field has nothing to frame around
    // and would otherwise be reframed to a default the moment it loaded.
    if (!line.fields.empty()) line.framed = true;
    if (!line.refs.empty() || !line.fields.empty()) lines.push_back(std::move(line));
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
    view.centre = loaded.centre;
    view.span = loaded.span;
    views.push_back(view);
  }
  shaders_.Adopt(std::move(views));

  std::vector<ColorView> colourOut;
  for (auto& [index, view] : colourViews) {
    if (index < 0 || index >= static_cast<int>(codeBlocks_.size())) continue;
    view.block = codeBlocks_[static_cast<std::size_t>(index)].get();
    colourOut.push_back(view);
  }
  colours_.Adopt(std::move(colourOut));

  std::vector<SurfaceView> surfaceOut;
  for (const LoadedTurned& loaded : surfaceViews) {
    if (loaded.block < 0 || loaded.block >= static_cast<int>(codeBlocks_.size())) continue;
    SurfaceView view;
    view.block = codeBlocks_[static_cast<std::size_t>(loaded.block)].get();
    view.docked = loaded.docked;
    view.worldPos = loaded.worldPos;
    view.centre = loaded.centre;
    view.span = loaded.span;
    view.orbit = loaded.orbit;
    surfaceOut.push_back(view);
  }
  surfaces_.Adopt(std::move(surfaceOut));

  std::vector<VolumeView> volumeOut;
  for (const LoadedTurned& loaded : volumeViews) {
    if (loaded.block < 0 || loaded.block >= static_cast<int>(codeBlocks_.size())) continue;
    VolumeView view;
    view.block = codeBlocks_[static_cast<std::size_t>(loaded.block)].get();
    view.docked = loaded.docked;
    view.worldPos = loaded.worldPos;
    view.centre = loaded.centre;
    view.span = loaded.span;
    view.orbit = loaded.orbit;
    volumeOut.push_back(view);
  }
  volumes_.Adopt(std::move(volumeOut));

  std::vector<SpaceWindow> spaceViews;
  for (const LoadedSpace& loaded : spaceWindows) {
    if (loaded.block < 0 || loaded.block >= static_cast<int>(codeBlocks_.size())) continue;
    SpaceWindow window;
    window.kind = (loaded.kind == 0) ? SpaceKind::Space : SpaceKind::Spacetime;
    window.dock = codeBlocks_[static_cast<std::size_t>(loaded.block)].get();
    window.docked = loaded.docked;
    window.worldPos = loaded.worldPos;
    window.yaw = loaded.yaw;
    window.pitch = loaded.pitch;
    window.dist = loaded.dist;
    window.cullOutside = loaded.cull;
    window.home = loaded.home;
    for (const auto& [index, name] : loaded.points) {
      if (index < 0 || index >= static_cast<int>(codeBlocks_.size())) continue;
      window.refs.push_back({codeBlocks_[static_cast<std::size_t>(index)].get(), name});
    }
    spaceViews.push_back(window);
  }
  spaces_.Adopt(std::move(spaceViews));

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
  // Here rather than in the menu, so a new canvas, a loaded one and an undo
  // step all set it the same way.
  SetAlwaysCode(alwaysCode != 0);
  slides_.Adopt(std::move(slides));
  if (!restoring) viewport_.Reset(centre, zoom);

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
  // Not for an undo step, which was written from a canvas already under both
  // rules -- and snapping again at whatever the zoom is now could move a box
  // that was put down finer than today's lattice.
  if (!restoring) {
    for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
      PlaceBlock(block.get(), block->WorldPosition());
    }
  }
  return true;
}

void App::NewCanvas() {
  FlushAutosave();
  std::istringstream blank("sketchbook 3\n");
  ReadCanvas(blank, std::string(), /*restoring=*/false);
  viewport_.Reset({0.0f, 0.0f}, kDefaultZoom);
  currentPath_.clear();
  untitled_ = false;
  autosaveDue_ = 0.0;
  ResetUndo();
  savedSnapshot_ = undoBase_;
}

// ------------------------------------------------------------------ undo

std::string App::Snapshot() {
  std::ostringstream out;
  WriteCanvas(out, currentPath_, /*withView=*/false);
  return out.str();
}

void App::ResetUndo() {
  undo_.clear();
  redo_.clear();
  undoBase_ = Snapshot();
  undoPending_ = false;
}

void App::CommitUndo() {
  if (!undoPending_) return;
  std::string now = Snapshot();
  if (now != undoBase_) {
    if (!undoBase_.empty()) {
      ScheduleAutosave();
      undo_.push_back(std::move(undoBase_));
      // A bound, like every other thing here that grows with use.
      constexpr std::size_t kUndoSteps = 200;
      if (undo_.size() > kUndoSteps) undo_.erase(undo_.begin());
      redo_.clear();
    }
    undoBase_ = std::move(now);
  }
  undoPending_ = false;
}

void App::TrackUndo(bool held) {
  const double now = GetTime();
  // Something being done. Movement alone is not: pointing changes nothing that
  // is saved, and counting it would turn a compile finishing under a moving
  // mouse into a step.
  const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
  const Vector2 wheel = GetMouseWheelMoveV();
  // A knob playing retypes its number every frame, and that is not something
  // being *done*: counting it would hold every undo step and every save back for
  // as long as anything plays, including edits made elsewhere meanwhile. So the
  // press of play is the step, like any press, and the swing after it is folded
  // into where things are like any other change nobody made -- except that when
  // it stops, where it stopped is written down.
  bool playing = false;
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) playing = playing || block->AnyPlaying();
  if (wasPlaying_ && !playing) {
    // Folded at once rather than on the next check a quarter of a second later:
    // otherwise the last stretch of the swing would become a step of its own,
    // and undo would go back to somewhere in the middle of it rather than to
    // where it started.
    undoBase_ = Snapshot();
    ScheduleAutosave();
  }
  wasPlaying_ = playing;
  const bool doing =
      IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ||
      IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) || IsMouseButtonReleased(MOUSE_BUTTON_LEFT) ||
      IsMouseButtonReleased(MOUSE_BUTTON_RIGHT) || IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE) ||
      wheel.x != 0.0f || wheel.y != 0.0f || touch::Count() > 0 || focus_ != nullptr ||
      IsKeyDown(KEY_DELETE) || (ctrl && (IsKeyDown(KEY_X) || IsKeyDown(KEY_V)));
  if (doing) {
    undoPending_ = true;
    undoLastInput_ = now;
  }

  // Wait until it has settled: nothing held, no box being written in, and a
  // moment since the last thing done -- which is what makes a run of wheel
  // clicks one step. Checked a few times a second rather than every frame.
  if (held || focus_ != nullptr || now - undoLastInput_ < 0.3 || now < undoNextCheck_) return;
  undoNextCheck_ = now + 0.25;

  if (undoPending_) {
    CommitUndo();
    return;
  }
  // Nobody did anything, and yet the canvas is different: a build came back, an
  // axis framed itself. That is where things are now, not a step.
  std::string current = Snapshot();
  if (current != undoBase_) undoBase_ = std::move(current);
}

void App::RestoreCanvas(const std::string& snapshot) {
  // An undo step like any other, so the rewind itself can be undone.
  CommitUndo();
  std::string current = Snapshot();
  std::istringstream in(snapshot);
  if (!ReadCanvas(in, currentPath_, /*restoring=*/true)) return;
  undo_.push_back(std::move(current));
  constexpr std::size_t kUndoSteps = 200;
  if (undo_.size() > kUndoSteps) undo_.erase(undo_.begin());
  redo_.clear();
  undoBase_ = Snapshot();
  undoPending_ = false;
  ScheduleAutosave();
}

void App::StepUndo(bool redo) {
  CommitUndo();
  std::vector<std::string>& from = redo ? redo_ : undo_;
  std::vector<std::string>& to = redo ? undo_ : redo_;
  if (from.empty()) return;
  std::string target = std::move(from.back());
  from.pop_back();
  std::istringstream in(target);
  if (!ReadCanvas(in, currentPath_, /*restoring=*/true)) return;
  to.push_back(std::move(undoBase_));
  undoBase_ = std::move(target);
  undoPending_ = false;
  ScheduleAutosave();
}

void App::TrackAutosave(bool held) {
  if (!autosaveOn_) return;
  const double now = GetTime();
  // Writing in a box is one undo step, taken when the caret leaves -- which
  // could be an hour of typing later. So while a box is being written in, the
  // canvas is saved every few seconds as well, if its text has moved.
  if (focus_ != nullptr && now - typingSavedAt_ > 3.0) {
    typingSavedAt_ = now;
    if (Snapshot() != savedSnapshot_) autosaveDue_ = now;
  }
  if (autosaveDue_ <= 0.0 || now < autosaveDue_ || held) return;
  autosaveDue_ = 0.0;

  if (currentPath_.empty()) {
    // Never saved and never opened: it gets a file of its own, among the
    // user's settings rather than wherever the app happens to be running.
    std::error_code ec;
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
#else
    const char* base = std::getenv("HOME");
#endif
    std::filesystem::path folder =
        (base != nullptr && *base != '\0') ? std::filesystem::path(base) / "sketchbook" / "untitled"
                                           : std::filesystem::path("untitled");
    std::filesystem::create_directories(folder, ec);
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M%S", std::localtime(&t));
    currentPath_ = (folder / (std::string("untitled ") + stamp + ".sketch")).string();
    untitled_ = true;
    recent::Remember(currentPath_);
  }
  if (!WriteCanvasFile(currentPath_)) {
    TraceLog(LOG_WARNING, "CANVAS: could not save %s", currentPath_.c_str());
  }
}

void App::FlushAutosave() {
  if (!autosaveOn_) return;
  // Leaving with a knob still swinging: its number is where it has got to, and
  // that is what goes in the file -- the swing itself is not kept.
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (block->AnyPlaying()) autosaveDue_ = GetTime();
  }
  // Anything done but not settled yet becomes a step, which asks for a save.
  CommitUndo();
  if (focus_ != nullptr && Snapshot() != savedSnapshot_) autosaveDue_ = GetTime();
  if (autosaveDue_ <= 0.0) return;
  autosaveDue_ = GetTime();
  TrackAutosave(/*held=*/false);
}

}  // namespace sb
