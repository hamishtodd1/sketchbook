#include "colordw.h"

#include <algorithm>
#include <cmath>

#include "grid.h"
#include "hover.h"
#include "rlgl.h"
#include "selection.h"
#include "text.h"
#include "theme.h"
#include "types/Color.h"

namespace sb {
namespace {

constexpr float kTau = 6.2831853f;

// Grey, so that black and white both read against it, and so the window says
// nothing about colour of its own.
constexpr Color kColorCardBg = {104, 104, 106, 255};
constexpr Color kColorCardEdge = {70, 70, 72, 255};

// The dropdown: chrome, so a fixed size on screen at any zoom.
constexpr float kMenuWidth = 136.0f;
constexpr float kMenuRow = 26.0f;
constexpr float kMenuGap = 8.0f;
constexpr float kMenuEm = 15.0f;
const char* const kShapeNames[] = {"Picker", "RGB cube", "HSV cylinder"};

Color ToColor(const float rgb[3], float alpha = 1.0f) {
  const auto channel = [](float v) {
    return static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
  };
  return {channel(rgb[0]), channel(rgb[1]), channel(rgb[2]),
          static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)};
}

Color HueColor(float hue) {
  const float hsv[3] = {hue, 1.0f, 1.0f};
  float       rgb[3];
  types::color::HsvToRgb(hsv, rgb);
  return ToColor(rgb);
}

// --- the picker -------------------------------------------------------------
//
// The hue ring, and inside it a triangle pointed at one hue: its corners are
// that hue, white and black, and a colour of that hue is the blend of the three
// by saturation and value -- `s v` of the hue, `(1 - s) v` of white, `1 - v` of
// black. Drawn with a colour at each corner, which the GPU blends in exactly
// that proportion.
struct Picker {
  Vector2 centre{};
  float   outer = 0.0f;
  float   inner = 0.0f;

  float   Mid() const { return (outer + inner) * 0.5f; }
  Vector2 At(float turns, float radius) const {
    const float a = turns * kTau;
    return {centre.x + std::cos(a) * radius, centre.y - std::sin(a) * radius};
  }
  // Pure hue, white, black.
  void Corners(float hue, Vector2 out[3]) const {
    const float r = inner * 0.95f;
    out[0] = At(hue, r);
    out[1] = At(hue + 1.0f / 3.0f, r);
    out[2] = At(hue - 1.0f / 3.0f, r);
  }
  float TurnsAt(Vector2 point) const {
    float turns = std::atan2(-(point.y - centre.y), point.x - centre.x) / kTau;
    return turns - std::floor(turns);
  }
};

Picker PickerFor(Rectangle rect) {
  Picker p;
  p.centre = {rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
  p.outer = std::min(rect.width, rect.height) * 0.45f;
  p.inner = p.outer * 0.82f;
  return p;
}

// Where a colour of this saturation and value sits in a triangle pointed at
// its hue.
Vector2 InTriangle(const Picker& picker, float hue, float s, float v) {
  Vector2 c[3];
  picker.Corners(hue, c);
  const float lp = s * v;
  const float lw = (1.0f - s) * v;
  const float lk = 1.0f - v;
  return {lp * c[0].x + lw * c[1].x + lk * c[2].x, lp * c[0].y + lw * c[1].y + lk * c[2].y};
}

// The saturation and value at a point, held inside the triangle.
void FromTriangle(const Picker& picker, float hue, Vector2 point, float* s, float* v) {
  Vector2 c[3];
  picker.Corners(hue, c);
  const float det = (c[1].y - c[2].y) * (c[0].x - c[2].x) + (c[2].x - c[1].x) * (c[0].y - c[2].y);
  float lp = ((c[1].y - c[2].y) * (point.x - c[2].x) + (c[2].x - c[1].x) * (point.y - c[2].y)) / det;
  float lw = ((c[2].y - c[0].y) * (point.x - c[2].x) + (c[0].x - c[2].x) * (point.y - c[2].y)) / det;
  float lk = 1.0f - lp - lw;
  lp = std::max(lp, 0.0f);
  lw = std::max(lw, 0.0f);
  lk = std::max(lk, 0.0f);
  const float sum = std::max(lp + lw + lk, 1e-6f);
  lp /= sum;
  lw /= sum;
  *v = lp + lw;
  *s = (*v > 1e-4f) ? lp / *v : 0.0f;
}

// --- the cube and the cylinder ---------------------------------------------
//
// Both are unit-sized and centred, so one camera frames either. Orthographic:
// the camera is two angles and nothing else.
Vector3 PointOf(ColorShape shape, const float rgb[3]) {
  if (shape == ColorShape::Cylinder) {
    float hsv[3];
    types::color::RgbToHsv(rgb, hsv);
    const float r = hsv[1] * 0.5f;
    return {r * std::cos(hsv[0] * kTau), hsv[2] - 0.5f, r * std::sin(hsv[0] * kTau)};
  }
  return {rgb[0] - 0.5f, rgb[1] - 0.5f, rgb[2] - 0.5f};
}

void ColourOf(ColorShape shape, Vector3 p, float rgb[3]) {
  if (shape == ColorShape::Cylinder) {
    float hue = std::atan2(p.z, p.x) / kTau;
    hue -= std::floor(hue);
    const float hsv[3] = {hue, std::sqrt(p.x * p.x + p.z * p.z) / 0.5f, p.y + 0.5f};
    types::color::HsvToRgb(hsv, rgb);
    return;
  }
  rgb[0] = p.x + 0.5f;
  rgb[1] = p.y + 0.5f;
  rgb[2] = p.z + 0.5f;
}

struct Camera2 {
  Vector2 centre{};
  float   scale = 1.0f;
  float   yaw = 0.0f;
  float   pitch = 0.0f;

  // Turned into the camera's frame: x across, y up, z towards the eye.
  Vector3 Turn(Vector3 p) const {
    const float x = std::cos(yaw) * p.x + std::sin(yaw) * p.z;
    const float z1 = -std::sin(yaw) * p.x + std::cos(yaw) * p.z;
    return {x, std::cos(pitch) * p.y - std::sin(pitch) * z1,
            std::sin(pitch) * p.y + std::cos(pitch) * z1};
  }
  Vector3 Unturn(Vector3 q) const {
    const float y = std::cos(pitch) * q.y + std::sin(pitch) * q.z;
    const float z1 = -std::sin(pitch) * q.y + std::cos(pitch) * q.z;
    return {std::cos(yaw) * q.x - std::sin(yaw) * z1, y, std::sin(yaw) * q.x + std::cos(yaw) * z1};
  }
  Vector2 Screen(Vector3 p, float* depth = nullptr) const {
    const Vector3 q = Turn(p);
    if (depth != nullptr) *depth = q.z;
    return {centre.x + q.x * scale, centre.y - q.y * scale};
  }
  // The point at this depth under a screen position: what a drag moves in.
  Vector3 Back(Vector2 screen, float depth) const {
    return Unturn({(screen.x - centre.x) / scale, -(screen.y - centre.y) / scale, depth});
  }
};

Camera2 CameraFor(const ColorView& view, Rectangle rect) {
  Camera2 camera;
  camera.centre = {rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
  camera.scale = std::min(rect.width, rect.height) * 0.52f;
  camera.yaw = view.yaw;
  camera.pitch = view.pitch;
  return camera;
}

// A line that changes colour along its length, as the colours between its ends
// do. Short straight pieces, each one colour: enough of them and it reads as a
// gradient, and every piece is geometry the window's multisampling smooths.
void GradientLine(Vector2 a, Vector2 b, const float ca[3], const float cb[3], float thick) {
  constexpr int kPieces = 16;
  for (int i = 0; i < kPieces; ++i) {
    const float t0 = static_cast<float>(i) / kPieces;
    const float t1 = static_cast<float>(i + 1) / kPieces;
    const float tm = (t0 + t1) * 0.5f;
    const float mid[3] = {ca[0] + (cb[0] - ca[0]) * tm, ca[1] + (cb[1] - ca[1]) * tm,
                          ca[2] + (cb[2] - ca[2]) * tm};
    DrawLineEx({a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0},
               {a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1}, thick, ToColor(mid));
  }
}

void DrawPicker(const Picker& picker, float hue) {
  rlDrawRenderBatchActive();
  rlDisableBackfaceCulling();
  // The ring, a colour at every vertex so the hue runs round it smoothly.
  constexpr int kSegments = 120;
  rlBegin(RL_TRIANGLES);
  for (int i = 0; i < kSegments; ++i) {
    const float t0 = static_cast<float>(i) / kSegments;
    const float t1 = static_cast<float>(i + 1) / kSegments;
    const Vector2 o0 = picker.At(t0, picker.outer), o1 = picker.At(t1, picker.outer);
    const Vector2 i0 = picker.At(t0, picker.inner), i1 = picker.At(t1, picker.inner);
    const Color   c0 = HueColor(t0), c1 = HueColor(t1);
    rlColor4ub(c0.r, c0.g, c0.b, 255); rlVertex2f(o0.x, o0.y);
    rlColor4ub(c1.r, c1.g, c1.b, 255); rlVertex2f(o1.x, o1.y);
    rlColor4ub(c0.r, c0.g, c0.b, 255); rlVertex2f(i0.x, i0.y);
    rlColor4ub(c1.r, c1.g, c1.b, 255); rlVertex2f(o1.x, o1.y);
    rlColor4ub(c1.r, c1.g, c1.b, 255); rlVertex2f(i1.x, i1.y);
    rlColor4ub(c0.r, c0.g, c0.b, 255); rlVertex2f(i0.x, i0.y);
  }
  // The triangle: the hue, white and black at its corners.
  Vector2 c[3];
  picker.Corners(hue, c);
  const Color pure = HueColor(hue);
  rlColor4ub(pure.r, pure.g, pure.b, 255); rlVertex2f(c[0].x, c[0].y);
  rlColor4ub(255, 255, 255, 255);          rlVertex2f(c[1].x, c[1].y);
  rlColor4ub(0, 0, 0, 255);                rlVertex2f(c[2].x, c[2].y);
  rlEnd();
  rlDrawRenderBatchActive();
  rlEnableBackfaceCulling();
}

void DrawCube(const Camera2& camera, float zoom) {
  const float thick = std::max(1.0f, 2.0f * zoom);
  for (int a = 0; a < 8; ++a) {
    for (int axis = 0; axis < 3; ++axis) {
      if ((a >> axis) & 1) continue;
      const int   b = a | (1 << axis);
      const float ca[3] = {static_cast<float>(a & 1), static_cast<float>((a >> 1) & 1),
                           static_cast<float>((a >> 2) & 1)};
      const float cb[3] = {static_cast<float>(b & 1), static_cast<float>((b >> 1) & 1),
                           static_cast<float>((b >> 2) & 1)};
      GradientLine(camera.Screen(PointOf(ColorShape::Cube, ca)),
                   camera.Screen(PointOf(ColorShape::Cube, cb)), ca, cb, thick);
    }
  }
}

void DrawCylinder(const Camera2& camera, float zoom) {
  const float thick = std::max(1.0f, 2.0f * zoom);
  constexpr int kSegments = 72;
  const float   black[3] = {0.0f, 0.0f, 0.0f};
  const float   white[3] = {1.0f, 1.0f, 1.0f};
  for (int i = 0; i < kSegments; ++i) {
    const float t0 = static_cast<float>(i) / kSegments;
    const float t1 = static_cast<float>(i + 1) / kSegments;
    const float h0[3] = {t0, 1.0f, 1.0f}, h1[3] = {t1, 1.0f, 1.0f};
    float       c0[3], c1[3];
    types::color::HsvToRgb(h0, c0);
    types::color::HsvToRgb(h1, c1);
    // The top rim runs through every hue; the bottom one is all black.
    GradientLine(camera.Screen(PointOf(ColorShape::Cylinder, c0)),
                 camera.Screen(PointOf(ColorShape::Cylinder, c1)), c0, c1, thick);
    const Vector3 b0 = {0.5f * std::cos(t0 * kTau), -0.5f, 0.5f * std::sin(t0 * kTau)};
    const Vector3 b1 = {0.5f * std::cos(t1 * kTau), -0.5f, 0.5f * std::sin(t1 * kTau)};
    DrawLineEx(camera.Screen(b0), camera.Screen(b1), thick, BLACK);
  }
  // Six walls' worth of edges, black at the bottom running up to their hue.
  for (int k = 0; k < 6; ++k) {
    const float t = static_cast<float>(k) / 6.0f;
    const float h[3] = {t, 1.0f, 1.0f};
    float       top[3];
    types::color::HsvToRgb(h, top);
    const Vector3 bottom = {0.5f * std::cos(t * kTau), -0.5f, 0.5f * std::sin(t * kTau)};
    GradientLine(camera.Screen(bottom), camera.Screen(PointOf(ColorShape::Cylinder, top)),
                 black, top, thick);
  }
  // And the greys up the middle.
  GradientLine(camera.Screen({0.0f, -0.5f, 0.0f}), camera.Screen({0.0f, 0.5f, 0.0f}), black,
               white, thick);
}

// Filled with the colour it is, and ringed, outside a gap, with the colour saying
// which variable it is. A sample's answer wears the same one.
void DrawMarker(Vector2 at, const float rgb[3], Color identity, float zoom) {
  const float r = std::max(4.0f, 7.0f * zoom);
  DrawCircleV(at, r, ToColor(rgb));
  DrawRing(at, r + 2.0f * std::max(zoom, 0.6f), r + 4.5f * std::max(zoom, 0.6f), 0.0f, 360.0f,
           36, identity);
}

}  // namespace

// --------------------------------------------------------------- the windows

void ColorPanel::Forget(const CodeBlock* block) {
  views_.erase(std::remove_if(views_.begin(), views_.end(),
                              [block](const ColorView& view) { return view.block == block; }),
               views_.end());
  drag_ = Drag::None;
  menuOpen_ = -1;
}

namespace {
// A box gets a colour window when it has a colour to show: a value of one, or a
// function giving one that is being fed.
bool HasColours(const CodeBlock& block) {
  for (const mathprogram::Variable& variable : block.Variables()) {
    if (variable.kind == mathprogram::Kind::Color) return true;
  }
  for (const mathprogram::Function& fn : block.Functions()) {
    if (fn.sampled && fn.outputType == types::color::kName) return true;
  }
  return false;
}
}  // namespace

bool ColorPanel::Sync(const Blocks& blocks) {
  const std::size_t before = views_.size();
  views_.erase(std::remove_if(views_.begin(), views_.end(),
                              [](const ColorView& view) {
                                return view.block == nullptr || !HasColours(*view.block) ||
                                       view.block->DrawnHidden(CodeBlock::kColour);
                              }),
               views_.end());
  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    if (!HasColours(*block) || block->DrawnHidden(CodeBlock::kColour)) continue;
    bool present = false;
    for (const ColorView& view : views_) {
      if (view.block == block.get()) { present = true; break; }
    }
    if (present) continue;
    ColorView view;
    view.block = block.get();
    views_.push_back(view);
  }
  if (menuOpen_ >= static_cast<int>(views_.size())) menuOpen_ = -1;
  return views_.size() != before;
}

int ColorPanel::FlankCount(const CodeBlock* block) const {
  int many = 0;
  for (const ColorView& view : views_) {
    if (view.docked && view.block == block) ++many;
  }
  return many;
}

Rectangle ColorPanel::RectFor(std::size_t index, const Viewport& viewport,
                              FontStore& fonts) const {
  const float      zoom = viewport.Zoom();
  const ColorView& view = views_[index];
  const float      side = kFlankSide * zoom;
  if (drag_ == Drag::Move && dragMoved_ && index == dragView_) {
    return {dragScreenPos_.x, dragScreenPos_.y, side, side};
  }
  if (!view.docked || view.block == nullptr) {
    const Vector2 topLeft = viewport.ToScreen(view.worldPos);
    return {topLeft.x, topLeft.y, side, side};
  }
  int slot = FlankBase(before_, view.block);
  for (std::size_t i = 0; i < index; ++i) {
    if (views_[i].docked && views_[i].block == view.block) ++slot;
  }
  return FlankRect(view.block->Measure(viewport, fonts).rect, slot, zoom);
}

Rectangle ColorPanel::DockedBounds(const CodeBlock* block, const Viewport& viewport,
                                   FontStore& fonts) const {
  for (std::size_t i = 0; i < views_.size(); ++i) {
    if (views_[i].block != block || !views_[i].docked) continue;
    if (drag_ == Drag::Move && dragMoved_ && i == dragView_) continue;
    return RectFor(i, viewport, fonts);
  }
  return {0.0f, 0.0f, 0.0f, 0.0f};
}

Rectangle ColorPanel::MenuRect(Rectangle window) const {
  return {window.x + window.width + kMenuGap, window.y, kMenuWidth, kMenuRow};
}

std::string ColorPanel::Focus(const ColorView& view) const {
  std::string first;
  std::string firstFree;
  for (const mathprogram::Variable& variable : view.block->Variables()) {
    if (variable.kind != mathprogram::Kind::Color) continue;
    if (variable.name == view.focus) return view.focus;
    if (first.empty()) first = variable.name;
    if (firstFree.empty() && variable.free) firstFree = variable.name;
  }
  return firstFree.empty() ? first : firstFree;
}

std::vector<ColorPanel::Marker> ColorPanel::Markers(std::size_t index, Rectangle rect) const {
  std::vector<Marker> out;
  const ColorView&    view = views_[index];
  if (view.block == nullptr) return out;
  const std::string focus = Focus(view);
  const Picker      picker = PickerFor(rect);
  const Camera2     camera = CameraFor(view, rect);
  std::vector<float> depths;

  for (const mathprogram::Variable& variable : view.block->Variables()) {
    if (variable.kind != mathprogram::Kind::Color || variable.components.size() != 3) continue;
    if (!view.block->InScope(variable.name)) continue;
    Marker marker;
    marker.name = variable.name;
    marker.free = variable.free;
    std::copy(variable.components.begin(), variable.components.end(), marker.rgb);
    float hsv[3];
    types::color::RgbToHsv(marker.rgb, hsv);

    if (view.shape == ColorShape::Picker) {
      // Every colour sits on the ring at its hue; the one the triangle points at
      // is also in the triangle, at its saturation and value.
      Marker ring = marker;
      ring.onRing = true;
      ring.at = picker.At(hsv[0], picker.Mid());
      out.push_back(ring);
      depths.push_back(-1.0f);
      if (variable.name == focus) {
        const float hue = (hsv[1] > 1e-3f) ? hsv[0] : view.focusHue;
        marker.at = InTriangle(picker, hue, hsv[1], hsv[2]);
        out.push_back(marker);
        depths.push_back(1.0f);
      }
      continue;
    }
    float depth = 0.0f;
    marker.at = camera.Screen(PointOf(view.shape, marker.rgb), &depth);
    out.push_back(marker);
    depths.push_back(depth);
  }

  // Far ones first, so the near ones are drawn over them.
  std::vector<std::size_t> order(out.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return depths[a] < depths[b]; });
  std::vector<Marker> sorted;
  sorted.reserve(out.size());
  for (std::size_t i : order) sorted.push_back(out[i]);
  return sorted;
}

int ColorPanel::MarkerAt(const std::vector<Marker>& markers, Vector2 point, float zoom) const {
  const float reach = std::max(8.0f, 11.0f * zoom);
  int         best = -1;
  float       bestDistance = reach;
  // Nearest wins; among equals, the one drawn on top -- the last.
  for (std::size_t i = 0; i < markers.size(); ++i) {
    const float d = std::hypot(point.x - markers[i].at.x, point.y - markers[i].at.y);
    if (d <= bestDistance) {
      bestDistance = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}

// ------------------------------------------------------------------ selection
void ColorPanel::Rects(const Viewport& viewport, FontStore& fonts,
                       std::vector<Rectangle>* out) const {
  AppendRects(views_, out, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}
void ColorPanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                              FontStore& fonts) {
  SelectCaught(views_, lasso, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}
void ColorPanel::ClearSelection() { ClearSelected(views_); }
bool ColorPanel::AnySelected() const { return sb::AnySelected(views_); }
bool ColorPanel::SelectedRimAt(Vector2 point, const Viewport& viewport, FontStore& fonts) const {
  return SelectedRim(views_, point, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}
void ColorPanel::BeginSelectionDrag() { RememberPositions(views_); }

void ColorPanel::MoveSelection(Vector2 worldDelta) {
  // A docked window has no position of its own until it is moved, so it is
  // given the one it is at before the selection carries it.
  MoveSelected(views_, worldDelta);
}

void ColorPanel::DeleteSelection() {
  for (const ColorView& view : views_) {
    if (view.selected && view.block != nullptr) view.block->HideDrawn(CodeBlock::kColour);
  }
  EraseSelected(views_);
  drag_ = Drag::None;
  menuOpen_ = -1;
}

void ColorPanel::DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const {
  for (std::size_t i = 0; i < views_.size(); ++i) {
    if (!views_[i].selected) continue;
    DrawRim(RectFor(i, viewport, fonts), viewport.Zoom(), true);
  }
}

void ColorPanel::ActiveVariables(const Viewport& viewport, FontStore& fonts,
                                 std::vector<ActiveValue>* out) const {
  if (out == nullptr) return;
  const Vector2 pointer = hover::Pointer();
  for (std::size_t i = 0; i < views_.size(); ++i) {
    if (views_[i].block == nullptr) continue;
    const Rectangle rect = RectFor(i, viewport, fonts);
    const std::vector<Marker> markers = Markers(i, rect);
    // The one held wins; otherwise the one under the pointer.
    if ((drag_ == Drag::Value || drag_ == Drag::Hue) && i == dragView_) {
      for (const Marker& marker : markers) {
        if (marker.name != dragName_) continue;
        ActiveValue value{views_[i].block, marker.name, marker.at};
        value.held = true;
        out->push_back(value);
        return;
      }
    }
    if (!CheckCollisionPointRec(pointer, rect)) continue;
    const int at = MarkerAt(markers, pointer, viewport.Zoom());
    if (at < 0) continue;
    out->push_back({views_[i].block, markers[static_cast<std::size_t>(at)].name,
                    markers[static_cast<std::size_t>(at)].at});
    return;
  }
}

// ---------------------------------------------------------------------- input

bool ColorPanel::HandleMenu(Rectangle window, std::size_t index) {
  const Vector2   pointer = GetMousePosition();
  const Rectangle head = MenuRect(window);
  const bool      open = (menuOpen_ == static_cast<int>(index));
  Rectangle       list = head;
  list.y += kMenuRow;
  list.height = kMenuRow * static_cast<float>(ColorShape::kCount);

  // It is only there to be pressed while it shows: while the pointer is on the
  // window or on the menu itself.
  const bool shows = open || CheckCollisionPointRec(pointer, window) ||
                     CheckCollisionPointRec(pointer, head);
  if (!shows) return false;
  const bool overMenu = CheckCollisionPointRec(pointer, head) ||
                        (open && CheckCollisionPointRec(pointer, list));
  if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return overMenu;

  if (CheckCollisionPointRec(pointer, head)) {
    menuOpen_ = open ? -1 : static_cast<int>(index);
    return true;
  }
  if (open && CheckCollisionPointRec(pointer, list)) {
    const int row = static_cast<int>((pointer.y - list.y) / kMenuRow);
    views_[index].shape = static_cast<ColorShape>(std::clamp(row, 0, 2));
    menuOpen_ = -1;
    return true;
  }
  if (open) menuOpen_ = -1;   // a press elsewhere puts it away, and goes on
  return false;
}

bool ColorPanel::HandleMenus(const Viewport& viewport, FontStore& fonts) {
  if (drag_ != Drag::None) return false;
  for (std::size_t i = 0; i < views_.size(); ++i) {
    if (views_[i].block == nullptr || views_[i].block->Absorbed()) continue;
    if (HandleMenu(RectFor(i, viewport, fonts), i)) return true;
  }
  return false;
}

bool ColorPanel::HandleInput(const Viewport& viewport, FontStore& fonts) {
  if (views_.empty()) return false;
  const Vector2 pointer = GetMousePosition();
  const float   zoom = viewport.Zoom();

  if (drag_ != Drag::None) {
    if (dragView_ >= views_.size() || views_[dragView_].block == nullptr) {
      drag_ = Drag::None;
      return false;
    }
    ColorView&      view = views_[dragView_];
    const Rectangle rect = RectFor(dragView_, viewport, fonts);
    switch (drag_) {
      case Drag::Move: {
        const Vector2 delta = GetMouseDelta();
        if (delta.x != 0.0f || delta.y != 0.0f) dragMoved_ = true;
        dragScreenPos_ = {pointer.x - dragGrab_.x, pointer.y - dragGrab_.y};
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
          if (dragMoved_ && OverDropTarget(pointer)) {
            view.block->HideDrawn(CodeBlock::kColour);
            views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(dragView_));
            menuOpen_ = -1;
          } else if (dragMoved_) {
            view.docked = false;
            view.worldPos = SnapToGrid(viewport.ToWorld(dragScreenPos_), zoom);
          }
          drag_ = Drag::None;
        }
        return true;
      }
      case Drag::Orbit: {
        // Dragging right turns the right-hand side towards you, as it does in
        // every window that turns.
        const Vector2 delta = GetMouseDelta();
        view.yaw -= delta.x * 0.01f;
        view.pitch = std::clamp(view.pitch + delta.y * 0.01f, -1.45f, 1.45f);
        if (!IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) drag_ = Drag::None;
        return true;
      }
      case Drag::Value:
      case Drag::Hue: {
        const mathprogram::Variable* variable = view.block->FindVariable(dragName_);
        if (variable != nullptr && variable->components.size() == 3) {
          float current[3] = {variable->components[0], variable->components[1],
                              variable->components[2]};
          float hsv[3];
          types::color::RgbToHsv(current, hsv);
          if (hsv[1] > 1e-3f) view.focusHue = hsv[0];
          float next[3] = {current[0], current[1], current[2]};
          if (view.shape == ColorShape::Picker) {
            const Picker picker = PickerFor(rect);
            if (drag_ == Drag::Hue) {
              view.focusHue = picker.TurnsAt(pointer);
              const float moved[3] = {view.focusHue, hsv[1], hsv[2]};
              types::color::HsvToRgb(moved, next);
            } else {
              float s = 0.0f, v = 0.0f;
              FromTriangle(picker, view.focusHue, pointer, &s, &v);
              const float moved[3] = {view.focusHue, s, v};
              types::color::HsvToRgb(moved, next);
            }
          } else {
            const Camera2 camera = CameraFor(view, rect);
            ColourOf(view.shape, camera.Back(pointer, dragDepth_), next);
          }
          view.block->SetColorValue(dragName_, next);
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) drag_ = Drag::None;
        return true;
      }
      case Drag::None:
        break;
    }
  }

  for (std::size_t i = 0; i < views_.size(); ++i) {
    ColorView& view = views_[i];
    if (view.block == nullptr || view.block->Absorbed()) continue;
    const Rectangle rect = RectFor(i, viewport, fonts);
    const bool      onRim = PointOnRim(rect, pointer);
    if (!CheckCollisionPointRec(pointer, rect) && !onRim) continue;

    // The right button turns the cube and the cylinder. The picker is flat and
    // has nothing to turn.
    if (!onRim && view.shape != ColorShape::Picker && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
      drag_ = Drag::Orbit;
      dragView_ = i;
      return true;
    }
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return true;   // hovering claims it
    if (onRim) {
      drag_ = Drag::Move;
      dragView_ = i;
      dragMoved_ = false;
      dragGrab_ = {pointer.x - rect.x, pointer.y - rect.y};
      return true;
    }

    const std::vector<Marker> markers = Markers(i, rect);
    const int                 at = MarkerAt(markers, pointer, zoom);
    const auto takeHold = [&](const std::string& name, Drag how) {
      view.block->StopPlaying(name);
      view.block->BeginValueEdit(name);
      drag_ = how;
      dragView_ = i;
      dragName_ = name;
    };
    if (at >= 0) {
      const Marker& marker = markers[static_cast<std::size_t>(at)];
      if (view.shape == ColorShape::Picker && marker.onRing) {
        // Pointing the triangle at a colour is choosing which one it shows.
        view.focus = marker.name;
        float hsv[3];
        types::color::RgbToHsv(marker.rgb, hsv);
        if (hsv[1] > 1e-3f) view.focusHue = hsv[0];
        if (marker.free) takeHold(marker.name, Drag::Hue);
        return true;
      }
      if (!marker.free) return true;   // derived: pointed at, not moved
      if (view.shape != ColorShape::Picker) {
        float depth = 0.0f;
        CameraFor(view, rect).Screen(PointOf(view.shape, marker.rgb), &depth);
        dragDepth_ = depth;
      }
      takeHold(marker.name, Drag::Value);
      return true;
    }
    // A press on the ring, off every marker, turns the one the triangle points
    // at to that hue -- the way every picker of this kind behaves.
    if (view.shape == ColorShape::Picker) {
      const Picker      picker = PickerFor(rect);
      const float       r = std::hypot(pointer.x - picker.centre.x, pointer.y - picker.centre.y);
      const std::string focus = Focus(view);
      const mathprogram::Variable* variable = view.block->FindVariable(focus);
      if (r >= picker.inner && r <= picker.outer && variable != nullptr && variable->free) {
        takeHold(focus, Drag::Hue);
      }
    }
    return true;
  }
  return false;
}

// ----------------------------------------------------------------------- draw

void ColorPanel::DrawMenu(Rectangle window, std::size_t index, FontStore& fonts) const {
  const Rectangle head = MenuRect(window);
  const bool      open = (menuOpen_ == static_cast<int>(index));
  const float     ascent = text::Ascent(fonts, Face::MainRegular, kMenuEm);
  const auto row = [&](Rectangle r, const char* label, bool hot) {
    DrawRectangleRec(r, hot ? Color{236, 238, 244, 255} : Color{250, 250, 250, 255});
    DrawRectangleLinesEx(r, 1.0f, Color{196, 198, 204, 255});
    text::Draw(fonts, Face::MainRegular, label,
               {r.x + 10.0f, r.y + (r.height + ascent * 0.8f) * 0.5f}, kMenuEm,
               Color{30, 30, 34, 255});
  };
  const Vector2 pointer = hover::Pointer();
  row(head, kShapeNames[static_cast<int>(views_[index].shape)],
      CheckCollisionPointRec(pointer, head));
  // Which way it opens, drawn rather than typed.
  const Vector2 tip = {head.x + head.width - 14.0f, head.y + head.height * 0.5f + 3.0f};
  DrawTriangle({tip.x - 5.0f, tip.y - 6.0f}, {tip.x, tip.y}, {tip.x + 5.0f, tip.y - 6.0f},
               Color{80, 80, 88, 255});
  if (!open) return;
  for (int k = 0; k < static_cast<int>(ColorShape::kCount); ++k) {
    const Rectangle r = {head.x, head.y + kMenuRow * static_cast<float>(k + 1), head.width,
                         kMenuRow};
    row(r, kShapeNames[k], CheckCollisionPointRec(pointer, r));
  }
}

void ColorPanel::Draw(const Viewport& viewport, FontStore& fonts) const {
  const float   zoom = viewport.Zoom();
  const Vector2 pointer = hover::Pointer();
  std::vector<ActiveValue> active;
  ActiveVariables(viewport, fonts, &active);

  for (std::size_t i = 0; i < views_.size(); ++i) {
    const ColorView& view = views_[i];
    if (view.block == nullptr || view.block->Absorbed()) continue;
    const bool carried = drag_ == Drag::Move && dragMoved_ && i == dragView_;
    if (carried && OverDropTarget(GetMousePosition())) continue;

    const Rectangle rect = RectFor(i, viewport, fonts);
    DrawRectangleRounded(rect, 0.03f, 8, kColorCardBg);
    DrawRectangleRoundedLinesEx(rect, 0.03f, 8, 1.0f, kColorCardEdge);

    BeginScissorMode(static_cast<int>(rect.x), static_cast<int>(rect.y),
                     static_cast<int>(rect.width), static_cast<int>(rect.height));
    if (view.shape == ColorShape::Picker) {
      const std::string focus = Focus(view);
      float             hue = view.focusHue;
      if (const mathprogram::Variable* v = view.block->FindVariable(focus);
          v != nullptr && v->components.size() == 3) {
        const float rgb[3] = {v->components[0], v->components[1], v->components[2]};
        float       hsv[3];
        types::color::RgbToHsv(rgb, hsv);
        if (hsv[1] > 1e-3f) hue = hsv[0];
      }
      const Picker picker = PickerFor(rect);
      DrawPicker(picker, hue);
      // A function giving a colour, fed a spread: its answers inside the ring at
      // their hues, which is all the picker has room to say about many colours.
      const Sampling& fed = view.block->Samples();
      if (fed.Live() && fed.outputType == types::color::kName && fed.outStride == 3) {
        for (int k = 0; k < fed.count; ++k) {
          const float* rgb = &fed.outputs[static_cast<std::size_t>(k * 3)];
          float        hsv[3];
          types::color::RgbToHsv(rgb, hsv);
          DrawMarker(picker.At(hsv[0], picker.inner - std::max(5.0f, 8.0f * zoom)), rgb,
                     view.block->VariableColor(fed.name), zoom);
        }
      }
    } else {
      const Camera2 camera = CameraFor(view, rect);
      if (view.shape == ColorShape::Cube) {
        DrawCube(camera, zoom);
      } else {
        DrawCylinder(camera, zoom);
      }
      // What a function giving a colour answers, for the spread it is fed: a
      // bead at each answer, the colour it is.
      const Sampling& fed = view.block->Samples();
      if (fed.Live() && fed.outputType == types::color::kName && fed.outStride == 3) {
        for (int k = 0; k < fed.count; ++k) {
          const float* rgb = &fed.outputs[static_cast<std::size_t>(k * 3)];
          DrawMarker(camera.Screen(PointOf(view.shape, rgb)), rgb,
                     view.block->VariableColor(fed.name), zoom);
        }
      }
    }
    for (const Marker& marker : Markers(i, rect)) {
      bool inUse = false;
      for (const ActiveValue& value : active) {
        inUse = inUse || (value.block == view.block && value.name == marker.name);
      }
      DrawMarker(marker.at, marker.rgb, Blink(view.block->VariableColor(marker.name), inUse),
                 zoom);
    }
    EndScissorMode();

    if (carried || PointOnRim(rect, pointer)) DrawRim(rect, zoom, carried);
  }
}

void ColorPanel::DrawChrome(const Viewport& viewport, FontStore& fonts) const {
  const Vector2 pointer = hover::Pointer();
  for (std::size_t i = 0; i < views_.size(); ++i) {
    if (views_[i].block == nullptr || views_[i].block->Absorbed()) continue;
    if (drag_ == Drag::Move && dragMoved_ && i == dragView_) continue;
    const Rectangle rect = RectFor(i, viewport, fonts);
    const bool      shows = menuOpen_ == static_cast<int>(i) ||
                       CheckCollisionPointRec(pointer, rect) ||
                       CheckCollisionPointRec(pointer, MenuRect(rect));
    if (shows) DrawMenu(rect, i, fonts);
  }
}

}  // namespace sb
