// A transition across a cut in an edited recording (DESIGN.md, *Recording and
// editing video*): what was there before the jump moving to what is there
// after it.
//
// The canvas shown is the one after the cut, built. Both ends are measured once
// -- where each box was, what its text looked like, what its knobs said -- and
// each frame the boxes are put part of the way from one to the other, the
// knobs set part of the way (as uniforms: nothing recompiles), and the camera
// part of the way. A box whose text changed has its own text left out, and a
// morph of the two drawn in its place (morph.h). What went is drawn as a ghost
// fading out; what came is uncovered.
#include <algorithm>
#include <cmath>
#include <sstream>

#include "screen.h"
#include "app.h"
#include "hover.h"
#include "screen.h"
#include "theme.h"

namespace sb {
namespace {

// How a box looked at one end of the cut, in world units.
struct Seen {
  Vector2                   at{};
  std::vector<std::string>  lines;
  std::vector<morph::Piece> text;   // relative to `at`
};

float Step(double from, double to, double t) {
  return morph::Ease(static_cast<float>((t - from) / (to - from)));
}

Vector2 Lerp(Vector2 a, Vector2 b, float s) { return {a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s}; }

}  // namespace

void App::LoadPlayed(const std::vector<std::string>& canvas) {
  std::string text;
  for (const std::string& line : canvas) text += line + '\n';
  std::istringstream in(text);
  deferBuilds_ = true;
  ReadCanvas(in, currentPath_, /*restoring=*/true);
  deferBuilds_ = false;
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (block->Focused()) block->SetFocused(false);
  }
  focus_ = nullptr;
  shownCanvas_ = canvas;
}

void App::ShowCut(const std::string& key, const CutEnd& before, const CutEnd& after,
                  double through, const std::vector<BoxShift>& shiftBefore,
                  const std::vector<BoxShift>& shiftAfter) {
  // The camera a view had, in this window: the same stretch of the world.
  const auto camera = [this](const recording::View& view, Vector2* centre, float* zoom) {
    const float scale =
        view.height > 0 ? static_cast<float>(ScreenHeight()) / view.height : 1.0f;
    *centre = {view.centreX, view.centreY};
    *zoom = view.zoom * scale;
  };

  if (!cut_.active || cut_.key != key) {
    EndCut();
    const auto look = [&](const CutEnd& end) {
      if (end.canvas != shownCanvas_) LoadPlayed(end.canvas);
      Vector2 centre{};
      float   zoom = 1.0f;
      camera(end.view, &centre, &zoom);
      viewport_.Reset(centre, zoom);
      std::vector<Seen> seen;
      for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
        Seen s;
        s.at = block->WorldPosition();
        s.lines = block->Lines();
        s.text = block->TextPieces(viewport_, fonts_);
        for (morph::Piece& piece : s.text) {
          for (morph::Contour& contour : piece.contours) {
            for (morph::Point& p : contour) {
              const Vector2 world = viewport_.ToWorld({p.x, p.y});
              p = {world.x - s.at.x, world.y - s.at.y};
            }
          }
        }
        seen.push_back(std::move(s));
      }
      return seen;
    };
    // Where each loose window is, panel by panel.
    using Loose = std::vector<std::vector<std::pair<int, Vector2>>>;
    const auto loose = [this]() {
      Loose out;
      for (Resizable* panel : Resizables()) {
        std::vector<std::pair<int, Vector2>>& mine = out.emplace_back();
        for (int i = 0; i < panel->GripCount(); ++i) {
          if (!panel->WindowDocked(i)) mine.emplace_back(i, panel->WindowWorldPos(i));
        }
      }
      return out;
    };
    const std::vector<Seen> a = look(before);
    const Loose             looseBefore = loose();
    // The canvas as it was, drawn once, for whatever goes to fade out of.
    RenderTexture2D shot{};
    std::vector<Rectangle> footprints;
    for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
      footprints.push_back(Footprint(block.get()));
    }
    {
      shot = LoadRenderTexture(ScreenWidth(), ScreenHeight());
      RenderTexture2D* outer = OuterTarget();
      OuterTarget() = &shot;
      BeginTextureMode(shot);
      ClearBackground(BLANK);
      DrawCanvas(/*look=*/true);
      EndTextureMode();
      OuterTarget() = outer;
      ResumeOuterTarget();
    }
    const auto onShot = [this](Rectangle world) {
      const Vector2 corner = viewport_.ToScreen({world.x, world.y});
      return Rectangle{corner.x, corner.y, world.width * viewport_.Zoom(),
                       world.height * viewport_.Zoom()};
    };
    std::vector<Rectangle> shotRects;
    for (const Rectangle& world : footprints) shotRects.push_back(onShot(world));
    const std::vector<Seen> b = look(after);
    const Loose             looseAfter = loose();

    Cut cut;
    cut.key = key;
    cut.before = before.view;
    cut.after = after.view;
    // Which box was which. The same text is the same box; otherwise the nearest
    // one left, if it is near enough to have been moved rather than replaced.
    std::vector<int>  partner(b.size(), -1);
    std::vector<bool> taken(a.size(), false);
    for (std::size_t j = 0; j < b.size(); ++j) {
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (!taken[i] && a[i].lines == b[j].lines) {
          partner[j] = static_cast<int>(i);
          taken[i] = true;
          break;
        }
      }
    }
    constexpr float kNearEnough = 800.0f;   // world units
    for (std::size_t j = 0; j < b.size(); ++j) {
      if (partner[j] >= 0) continue;
      float best = kNearEnough;
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (taken[i]) continue;
        const float d = std::hypot(a[i].at.x - b[j].at.x, a[i].at.y - b[j].at.y);
        if (d < best) {
          best = d;
          partner[j] = static_cast<int>(i);
        }
      }
      if (partner[j] >= 0) taken[static_cast<std::size_t>(partner[j])] = true;
    }

    // The edit's own moves of boxes, at either end, are where they go from and to.
    const auto shiftOf = [](const std::vector<BoxShift>& shifts, int index) {
      for (const BoxShift& shift : shifts) {
        if (shift.index == index) return shift;
      }
      return BoxShift{index, 0.0f, 0.0f, 1.0f};
    };
    const std::vector<recording::BoxPlace> placesA = recording::Boxes(before.canvas);
    const std::vector<recording::BoxPlace> placesB = recording::Boxes(after.canvas);
    for (std::size_t j = 0; j < b.size(); ++j) {
      CutBox box;
      box.block = codeBlocks_[j].get();
      const BoxShift toShift = shiftOf(shiftAfter, static_cast<int>(j));
      box.to = {b[j].at.x + toShift.dx, b[j].at.y + toShift.dy};
      box.emTo = (j < placesB.size() ? placesB[j].em : box.block->Em()) * toShift.scale;
      box.emFrom = box.emTo;
      const int i = partner[j];
      if (i < 0) {
        box.from = box.to;
        box.arrives = true;
      } else {
        const Seen&    was = a[static_cast<std::size_t>(i)];
        const BoxShift fromShift = shiftOf(shiftBefore, i);
        box.from = {was.at.x + fromShift.dx, was.at.y + fromShift.dy};
        if (static_cast<std::size_t>(i) < placesA.size()) {
          box.emFrom = placesA[static_cast<std::size_t>(i)].em * fromShift.scale;
        }
        if (was.lines != b[j].lines) {
          box.morphs = true;
          box.text = morph::Morph(was.text, b[j].text);
        }
        for (const auto& [name, value] : recording::BoxValues(before.canvas, i)) {
          for (const auto& [later, now] : recording::BoxValues(after.canvas, static_cast<int>(j))) {
            if (later == name && now != value) box.values.emplace_back(name, value, now);
          }
        }
      }
      box.block->SetTextHidden(box.morphs);
      box.block->SetOverlayHidden(box.morphs || box.arrives);
      cut.boxes.push_back(std::move(box));
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (taken[i] || i >= footprints.size()) continue;
      cut.ghosts.push_back({footprints[i], shotRects[i]});
    }
    if (cut.ghosts.empty()) {
      UnloadRenderTexture(shot);
    } else {
      cut.shot = shot;
    }
    // Loose windows: the k-th loose one of a kind before is the k-th after.
    const std::vector<Resizable*> panels = Resizables();
    for (std::size_t p = 0; p < panels.size() && p < looseBefore.size() && p < looseAfter.size(); ++p) {
      if (looseBefore[p].size() != looseAfter[p].size()) continue;
      for (std::size_t k = 0; k < looseAfter[p].size(); ++k) {
        cut.windows.push_back({panels[p], looseAfter[p][k].first, looseBefore[p][k].second,
                               looseAfter[p][k].second});
      }
    }
    // The canvas after the cut is the one shown, so it is built, as a jump is.
    for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) block->RequestCompile();
    cut.active = true;
    cut_ = std::move(cut);
  }

  // The two views come every frame: the editor's keys can move them.
  cut_.before = before.view;
  cut_.after = after.view;
  cut_.through = std::clamp(through, 0.0, 1.0);
  const float s = morph::Ease(static_cast<float>(cut_.through));
  Vector2 fromCentre{}, toCentre{};
  float   fromZoom = 1.0f, toZoom = 1.0f;
  camera(cut_.before, &fromCentre, &fromZoom);
  camera(cut_.after, &toCentre, &toZoom);
  // Zoom eases in proportion, so a journey in looks the same as one out.
  viewport_.Reset(Lerp(fromCentre, toCentre, s),
                  std::exp(std::log(fromZoom) + (std::log(toZoom) - std::log(fromZoom)) * s));
  for (CutBox& box : cut_.boxes) {
    box.block->SetWorldPosition(Lerp(box.from, box.to, s));
    if (box.emTo > 0.0f && box.emFrom > 0.0f) {
      box.block->SetEm(std::exp(std::log(box.emFrom) + (std::log(box.emTo) - std::log(box.emFrom)) * s));
    }
    for (const auto& [name, from, to] : box.values) {
      if (box.block->FindVariable(name) != nullptr) {
        box.block->SetVariableValue(name, from + (to - from) * s, /*intoText=*/false);
      }
    }
  }
  for (const CutWindow& window : cut_.windows) {
    if (window.index < window.panel->GripCount()) {
      window.panel->SetWindowWorldPos(window.index, Lerp(window.from, window.to, s));
    }
  }
  // Nothing is pointed at while it moves.
  hover::SetReplayed(true, {-1.0e9f, -1.0e9f});
  replayHeld_ = true;
}

void App::EndCut() {
  if (!cut_.active) return;
  for (const CutWindow& window : cut_.windows) {
    if (window.index < window.panel->GripCount()) window.panel->SetWindowWorldPos(window.index, window.to);
  }
  if (cut_.shot.id != 0) UnloadRenderTexture(cut_.shot);
  for (CutBox& box : cut_.boxes) {
    box.block->SetWorldPosition(box.to);
    if (box.emTo > 0.0f) box.block->SetEm(box.emTo);
    box.block->SetTextHidden(false);
    box.block->SetOverlayHidden(false);
    for (const auto& [name, from, to] : box.values) {
      if (box.block->FindVariable(name) != nullptr) {
        box.block->SetVariableValue(name, to, /*intoText=*/false);
      }
    }
  }
  cut_ = {};
}

void App::ShiftBoxes(const std::vector<BoxShift>& shifts) {
  // In a transition the boxes are already being put where they go.
  if (cut_.active) return;
  const std::vector<recording::BoxPlace> places = recording::Boxes(shownCanvas_);
  if (places.size() != codeBlocks_.size()) return;
  shifted_.resize(codeBlocks_.size(), false);
  for (std::size_t i = 0; i < codeBlocks_.size(); ++i) {
    BoxShift shift{static_cast<int>(i), 0.0f, 0.0f, 1.0f};
    for (const BoxShift& s : shifts) {
      if (s.index == static_cast<int>(i)) shift = s;
    }
    const bool moved = shift.dx != 0.0f || shift.dy != 0.0f || shift.scale != 1.0f;
    // One moved last frame and not now goes back where the canvas has it.
    if (!moved && !shifted_[i]) continue;
    codeBlocks_[i]->SetWorldPosition({places[i].x + shift.dx, places[i].y + shift.dy});
    codeBlocks_[i]->SetEm(places[i].em * shift.scale);
    shifted_[i] = moved;
  }
}

int App::BoxUnder(Vector2 point) {
  // The topmost: the last drawn.
  for (int i = static_cast<int>(codeBlocks_.size()) - 1; i >= 0; --i) {
    if (CheckCollisionPointRec(point, BoxRect(i))) return i;
  }
  return -1;
}

Rectangle App::BoxRect(int index) {
  if (index < 0 || index >= static_cast<int>(codeBlocks_.size())) return {};
  return codeBlocks_[static_cast<std::size_t>(index)]->Measure(viewport_, fonts_).rect;
}

long long App::BoxId(int index) const {
  if (index < 0 || index >= static_cast<int>(codeBlocks_.size())) return 0;
  return codeBlocks_[static_cast<std::size_t>(index)]->SavedId();
}

void App::DrawCut() {
  if (!cut_.active) return;
  if (!cutFill_.Ready() && !cutFill_.Load()) return;
  const double t = cut_.through;
  const float  s = morph::Ease(static_cast<float>(t));
  const float  zoom = viewport_.Zoom();
  const auto   screenRect = [this, zoom](Rectangle world) {
    const Vector2 corner = viewport_.ToScreen({world.x, world.y});
    return Rectangle{corner.x, corner.y, world.width * zoom, world.height * zoom};
  };
  const auto onScreen = [this](std::vector<morph::Shape>* shapes, Vector2 offset) {
    for (morph::Shape& shape : *shapes) {
      for (morph::Contour& contour : shape.contours) {
        for (morph::Point& p : contour) {
          const Vector2 screen = viewport_.ToScreen({p.x + offset.x, p.y + offset.y});
          p = {screen.x, screen.y};
        }
      }
    }
  };

  // What went: the box and its windows, as they were, fading where they were.
  const float going = 1.0f - Step(0.0, 0.6, t);
  for (const CutGhost& ghost : cut_.ghosts) {
    if (going <= 0.0f || cut_.shot.id == 0) break;
    // A render texture is upside down.
    const Rectangle from = {ghost.shot.x, static_cast<float>(cut_.shot.texture.height) - ghost.shot.y -
                                              ghost.shot.height,
                            ghost.shot.width, -ghost.shot.height};
    DrawTexturePro(cut_.shot.texture, from, screenRect(ghost.world), {0.0f, 0.0f}, 0.0f,
                   Fade(WHITE, going));
  }

  for (const CutBox& box : cut_.boxes) {
    const Vector2 at = Lerp(box.from, box.to, s);
    if (box.morphs) {
      std::vector<morph::Shape> shapes = box.text.At(static_cast<float>(t));
      onScreen(&shapes, at);
      cutFill_.Draw(shapes, box.block->Prose() ? kSlideInk : kCodeFg);
    }
    // What came is uncovered: the canvas over it and its windows, thinning.
    if (box.arrives) {
      const float  hidden = 1.0f - Step(0.4, 1.0, t);
      Rectangle    r = screenRect(Footprint(box.block));   // a footprint is in the world
      const float  pad = 6.0f * zoom;
      r = {r.x - pad, r.y - pad, r.width + 2.0f * pad, r.height + 2.0f * pad};
      if (hidden > 0.0f) DrawRectangleRec(r, Fade(kCanvasBg, hidden));
    }
  }
}

}  // namespace sb
