#include "spacedw.h"

#include <algorithm>
#include <cmath>

#include "grid.h"
#include "raymath.h"
#include "rlgl.h"
#include "theme.h"

namespace sb {
namespace {

// How big one of these is on the canvas, and how far it sits from its box.
constexpr float kSide = 300.0f;
constexpr float kGap = 12.0f;

// The cuboid. Not a box containing the space -- a window into it: what falls
// outside is cut away, so the walls are where the world stops being drawn.
constexpr float kLimitX = 3.9f;
constexpr float kLimitY = 2.1f;
constexpr float kLimitZ = 3.9f;

// Where a point at infinity is drawn. It is nowhere in particular, so it is put
// far enough out to read as a direction rather than as a place.
constexpr float kInfinityRadius = 40.0f;
constexpr float kPointRadius = 0.22f;

// A trivector is a point, and its four coefficients are that point's
// homogeneous coordinates. Which coefficient is which axis needs no table: a
// grade-3 blade of a 4-generator algebra *omits* exactly one generator, and the
// one it omits is the axis it stands for. So e023 omits e1 and is x, e013 omits
// e2 and is y, e012 omits e3 and is z, and e123 omits the null generator e0 and
// is therefore w -- which is the rule stated plainly: the coefficient with no
// null element is the homogeneous one.
//
// Components arrive in the order the struct declares them, which is ascending
// blade index: e012, e013, e023, e123.
//
// KNOWN: taken with the signs as written. The strict dual alternates them, so
// if points come out mirrored in x and z this is the one line to change.
struct Point {
  Vector3 at{};
  bool    atInfinity = false;
  bool    known = false;
};

Point PointOf(const std::vector<float>& c) {
  Point p;
  if (c.size() < 4) return p;
  p.known = true;
  const float w = c[3];
  const Vector3 dir = {c[2], c[1], c[0]};
  if (std::abs(w) < 1e-6f) {
    // No position, only a direction. Put it out at the edge of everything.
    const float length = Vector3Length(dir);
    p.atInfinity = true;
    p.at = (length > 1e-9f) ? Vector3Scale(dir, kInfinityRadius / length) : Vector3{0.0f, 0.0f, 0.0f};
    return p;
  }
  p.at = Vector3Scale(dir, 1.0f / w);
  return p;
}

// Everything drawn in here shares one vertex shader. It is raylib's own, plus
// the world position the limits are tested against.
const char* kVertexShader = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;

uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;

out vec3 fragPosition;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;

void main() {
    fragPosition = vec3(matModel * vec4(vertexPosition, 1.0));
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 1.0)));
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

// One directional light, and the cut. A fragment outside the cuboid is simply
// not there -- unless this draw is exempt, which is what lets a point at
// infinity and the walls themselves survive a test they would otherwise fail.
const char* kSolidShader = R"(#version 330
in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;

uniform vec4 colDiffuse;
uniform vec3 viewPos;
uniform vec3 limitsMin;
uniform vec3 limitsMax;
uniform float cullOutside;
uniform float exempt;

out vec4 finalColor;

void main() {
    bool inside = all(greaterThan(fragPosition, limitsMin)) &&
                  all(lessThan(fragPosition, limitsMax));
    if (cullOutside > 0.5 && exempt < 0.5 && !inside) discard;

    vec3 normal = normalize(fragNormal);
    vec3 toEye = normalize(viewPos - fragPosition);
    // Lit from over the viewer's shoulder, so a surface never goes black just
    // because the camera came round to the other side of it.
    vec3 toLight = normalize(vec3(0.4, 0.85, 0.35));
    float lambert = max(dot(normal, toLight), 0.0);
    float back = max(dot(normal, -toLight), 0.0) * 0.25;
    float spec = pow(max(dot(toEye, reflect(-toLight, normal)), 0.0), 24.0) * 0.35;

    vec3 base = (colDiffuse * fragColor).rgb;
    vec3 lit = base * (0.35 + 0.75 * lambert + back) + vec3(spec);
    finalColor = vec4(pow(lit, vec3(1.0 / 2.2)), colDiffuse.a * fragColor.a);
}
)";

// The sky. Both are a latitude ramp with white overhead and black underfoot;
// spacetime additionally banded by longitude, with a hard horizon where the
// light cone would cut it.
const char* kSkyCommon = R"(#version 330
in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;
out vec4 finalColor;

vec3 hueToRgb(float hue) {
    hue = fract(hue);
    float r = abs(hue * 6.0 - 3.0) - 1.0;
    float g = 2.0 - abs(hue * 6.0 - 2.0);
    float b = 2.0 - abs(hue * 6.0 - 4.0);
    return clamp(vec3(r, g, b), 0.0, 1.0);
}
)";

const char* kSkySpace = R"(
void main() {
    vec3  r = normalize(fragPosition);
    float latitude = atan(r.y, sqrt(r.x * r.x + r.z * r.z));

    vec3  skyBlue = vec3(173.0 / 255.0, 222.0 / 255.0, 254.0 / 255.0);
    vec3  poleCol = latitude > 0.0 ? vec3(1.0) : vec3(0.0);
    float poleness = latitude / (0.5 * 3.14159265359);
    finalColor = vec4(mix(skyBlue, poleCol, abs(poleness)), 1.0);
}
)";

const char* kSkySpacetime = R"(
void main() {
    vec3  r = normalize(fragPosition);
    float d = sqrt(r.x * r.x + r.z * r.z);
    float latitude = atan(r.y, d);
    float longitude = atan(r.z, r.x);

    vec3 skyBlue   = vec3(173.0 / 255.0, 222.0 / 255.0, 254.0 / 255.0);
    vec3 skyOrange = vec3(247.0 / 255.0, 117.0 / 255.0, 0.0);
    vec3 lonColor  = abs(latitude) < 0.25 * 3.14159265359 ? skyBlue : skyOrange;

    vec3  poleCol = latitude > 0.0 ? vec3(1.0) : vec3(0.0);
    float poleness = latitude / (0.5 * 3.14159265359);
    vec3  c = mix(lonColor, poleCol, abs(poleness));

    // Where the cone meets the sky.
    if (0.003 > abs(abs(latitude) - 0.25 * 3.14159265359)) c = vec3(0.0);
    finalColor = vec4(c, 1.0);
}
)";

// Turned inside out: raylib culls back faces, so a cube seen from within is
// invisible until its winding and its normals are reversed. That is what makes
// the cuboid something you are *inside* -- the near walls fall away and you
// always see into it, whichever way the camera has come round.
void TurnInsideOut(Mesh* mesh) {
  if (mesh == nullptr) return;
  if (mesh->indices != nullptr) {
    for (int i = 0; i + 2 < mesh->triangleCount * 3; i += 3) {
      std::swap(mesh->indices[i + 1], mesh->indices[i + 2]);
    }
  } else if (mesh->vertices != nullptr) {
    for (int i = 0; i + 2 < mesh->vertexCount; i += 3) {
      for (int k = 0; k < 3; ++k) {
        std::swap(mesh->vertices[(i + 1) * 3 + k], mesh->vertices[(i + 2) * 3 + k]);
        if (mesh->normals != nullptr) {
          std::swap(mesh->normals[(i + 1) * 3 + k], mesh->normals[(i + 2) * 3 + k]);
        }
      }
    }
  }
  if (mesh->normals != nullptr) {
    for (int i = 0; i < mesh->vertexCount * 3; ++i) mesh->normals[i] = -mesh->normals[i];
  }

  // And up to the card, or none of the above has happened as far as the
  // drawing is concerned. An indexed mesh keeps its winding in the *index*
  // buffer -- raylib's cube is indexed and its sphere is not -- so uploading
  // only the vertices and normals turns the cube's lighting round while
  // leaving it facing exactly the way it was.
  UpdateMeshBuffer(*mesh, 0, mesh->vertices, mesh->vertexCount * 3 * sizeof(float), 0);
  if (mesh->normals != nullptr) {
    UpdateMeshBuffer(*mesh, 2, mesh->normals, mesh->vertexCount * 3 * sizeof(float), 0);
  }
  if (mesh->indices != nullptr) {
    UpdateMeshBuffer(*mesh, 6, mesh->indices,
                     mesh->triangleCount * 3 * static_cast<int>(sizeof(unsigned short)), 0);
  }
}

bool LassoCatches(const std::vector<Vector2>& lasso, Rectangle rect) {
  const Vector2 middle = {rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
  bool inside = false;
  for (std::size_t i = 0, j = lasso.size() - 1; i < lasso.size(); j = i++) {
    if ((lasso[i].y > middle.y) == (lasso[j].y > middle.y)) continue;
    const float x = (lasso[j].x - lasso[i].x) * (middle.y - lasso[i].y) /
                        (lasso[j].y - lasso[i].y) +
                    lasso[i].x;
    if (middle.x < x) inside = !inside;
  }
  return inside;
}

}  // namespace

clifford::Signature SignatureOf(SpaceKind kind) {
  return (kind == SpaceKind::Space) ? clifford::Signature{3, 0, 1}
                                    : clifford::Signature{2, 1, 1};
}

SpaceKind KindOfSignature(const clifford::Signature& sig, bool* found) {
  if (sig.p == 3 && sig.q == 0 && sig.r == 1) {
    if (found != nullptr) *found = true;
    return SpaceKind::Space;
  }
  if (sig.p == 2 && sig.q == 1 && sig.r == 1) {
    if (found != nullptr) *found = true;
    return SpaceKind::Spacetime;
  }
  if (found != nullptr) *found = false;
  return SpaceKind::Space;
}

SpacePanel::~SpacePanel() {
  if (haveGpu_) {
    TraceLog(LOG_WARNING, "SPACE: GPU resources were still held at destruction");
  }
}

void SpacePanel::ReleaseGpu() {
  if (!haveGpu_) return;

  // raylib frees a material's shader along with its model, and these models
  // share their shaders -- two of them are the same `solid_`, and the sky's is
  // swapped between two as it is drawn. Left alone, unloading the models would
  // free the same shader two and three times over. So the models are handed
  // back the default shader first and the three real ones are released here,
  // once each, by the code that made them.
  const unsigned int fallback = rlGetShaderIdDefault();
  for (Model* model : {&skyBall_, &ball_, &wall_}) {
    if (model->materialCount > 0) model->materials[0].shader.id = fallback;
  }
  UnloadModel(skyBall_);
  UnloadModel(ball_);
  UnloadModel(wall_);

  UnloadShader(sky_[0]);
  UnloadShader(sky_[1]);
  UnloadShader(solid_);
  UnloadRenderTexture(target_);
  haveGpu_ = false;
  targetSide_ = 0;
}

void SpacePanel::EnsureGpu() {
  if (haveGpu_) return;

  solid_ = LoadShaderFromMemory(kVertexShader, kSolidShader);
  sky_[0] = LoadShaderFromMemory(kVertexShader,
                                 (std::string(kSkyCommon) + kSkySpace).c_str());
  sky_[1] = LoadShaderFromMemory(kVertexShader,
                                 (std::string(kSkyCommon) + kSkySpacetime).c_str());

  solid_.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(solid_, "viewPos");
  limitsMinLoc_ = GetShaderLocation(solid_, "limitsMin");
  limitsMaxLoc_ = GetShaderLocation(solid_, "limitsMax");
  cullLoc_ = GetShaderLocation(solid_, "cullOutside");
  atInfinityLoc_ = GetShaderLocation(solid_, "exempt");

  ball_ = LoadModelFromMesh(GenMeshSphere(1.0f, 12, 16));
  ball_.materials[0].shader = solid_;

  // The sky is a sphere seen from inside, and so is the cuboid.
  skyBall_ = LoadModelFromMesh(GenMeshSphere(kInfinityRadius * 1.6f, 10, 16));
  TurnInsideOut(&skyBall_.meshes[0]);

  wall_ = LoadModelFromMesh(GenMeshCube(1.0f, 1.0f, 1.0f));
  TurnInsideOut(&wall_.meshes[0]);
  wall_.materials[0].shader = solid_;

  // One target, shared by every window: drawn into and blitted at once, so the
  // next window can have it straight back.
  targetSide_ = 512;
  target_ = LoadRenderTexture(targetSide_, targetSide_);
  haveGpu_ = true;
}

Rectangle SpacePanel::RectFor(std::size_t index, const Viewport& viewport,
                              FontStore& fonts) const {
  const float zoom = viewport.Zoom();
  const SpaceWindow& window = windows_[index];
  const float side = kSide * zoom;

  if (dragging_ && dragMoved_ && index == dragView_) {
    return {dragScreenPos_.x, dragScreenPos_.y, side, side};
  }
  if (!window.docked || window.dock == nullptr) {
    const Vector2 topLeft = viewport.ToScreen(window.worldPos);
    return {topLeft.x, topLeft.y, side, side};
  }
  const Rectangle card = window.dock->Measure(viewport, fonts).rect;
  return {card.x + card.width + kGap * zoom, card.y, side, side};
}

Rectangle SpacePanel::DockedBounds(const CodeBlock* block, const Viewport& viewport,
                                   FontStore& fonts) const {
  for (std::size_t i = 0; i < windows_.size(); ++i) {
    if (windows_[i].dock != block || !windows_[i].docked) continue;
    if (dragging_ && dragMoved_ && i == dragView_) continue;
    return RectFor(i, viewport, fonts);
  }
  return {0.0f, 0.0f, 0.0f, 0.0f};
}

bool SpacePanel::Sync(const Blocks& blocks) {
  bool changed = false;

  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    bool             found = false;
    const SpaceKind  kind = KindOfSignature(block->Algebra(), &found);
    if (!found) continue;

    // Every point the box declares, in the order it declares them.
    std::vector<PointRef> wanted;
    for (const mathprogram::Variable& variable : block->Variables()) {
      if (variable.kind != mathprogram::Kind::Multivector) continue;
      if (variable.type != "Tri") continue;
      wanted.push_back({block.get(), variable.name});
    }

    std::size_t existing = windows_.size();
    for (std::size_t i = 0; i < windows_.size(); ++i) {
      if (windows_[i].dock == block.get()) { existing = i; break; }
    }

    if (wanted.empty()) {
      if (existing < windows_.size()) {
        windows_.erase(windows_.begin() + static_cast<long>(existing));
        changed = true;
      }
      continue;
    }

    if (existing == windows_.size()) {
      SpaceWindow window;
      window.kind = kind;
      window.dock = block.get();
      window.refs = std::move(wanted);
      windows_.push_back(std::move(window));
      changed = true;
      continue;
    }

    SpaceWindow& window = windows_[existing];
    if (window.kind != kind) { window.kind = kind; changed = true; }
    if (window.refs.size() != wanted.size()) changed = true;
    window.refs = std::move(wanted);
  }

  // A window whose box has gone goes with it.
  for (std::size_t i = windows_.size(); i-- > 0;) {
    bool alive = false;
    for (const std::unique_ptr<CodeBlock>& block : blocks) {
      if (block.get() == windows_[i].dock) { alive = true; break; }
    }
    if (alive) continue;
    windows_.erase(windows_.begin() + static_cast<long>(i));
    changed = true;
  }
  return changed;
}

void SpacePanel::Forget(const CodeBlock* block) {
  for (std::size_t i = windows_.size(); i-- > 0;) {
    if (windows_[i].dock != block) continue;
    windows_.erase(windows_.begin() + static_cast<long>(i));
  }
  for (SpaceWindow& window : windows_) {
    for (std::size_t i = window.refs.size(); i-- > 0;) {
      if (window.refs[i].block != block) continue;
      window.refs.erase(window.refs.begin() + static_cast<long>(i));
    }
  }
  dragging_ = false;
  orbiting_ = false;
}

bool SpacePanel::HandleInput(const Viewport& viewport, FontStore& fonts) {
  // Two fingers turn the camera, which is the tablet's version of the right
  // button -- and the reason it can be: over one of these, two fingers have
  // nothing else to mean. Elsewhere on the canvas they still pinch.
  const int fingers = touch::Count();
  if (fingers == 2) {
    const touch::Point* points = touch::Points();
    const Vector2 middle = {(points[0].position.x + points[1].position.x) * 0.5f,
                            (points[0].position.y + points[1].position.y) * 0.5f};
    if (!touchOrbit_) {
      for (std::size_t i = 0; i < windows_.size(); ++i) {
        if (!CheckCollisionPointRec(middle, RectFor(i, viewport, fonts))) continue;
        touchOrbit_ = true;
        orbitView_ = i;
        orbitFrom_ = middle;
        break;
      }
    } else if (orbitView_ < windows_.size()) {
      SpaceWindow& window = windows_[orbitView_];
      window.yaw -= (middle.x - orbitFrom_.x) * 0.01f;
      window.pitch -= (middle.y - orbitFrom_.y) * 0.01f;
      window.pitch = std::clamp(window.pitch, -PI / 2.01f, PI / 2.01f);
      orbitFrom_ = middle;
    }
    if (touchOrbit_) return true;
  } else {
    touchOrbit_ = false;
  }

  // An orbit in flight owns the pointer until the button comes up, exactly as
  // every other drag here does.
  if (orbiting_) {
    if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT) || orbitView_ >= windows_.size()) {
      orbiting_ = false;
      return true;
    }
    SpaceWindow& window = windows_[orbitView_];
    const Vector2 now = GetMousePosition();
    window.yaw -= (now.x - orbitFrom_.x) * 0.01f;
    window.pitch -= (now.y - orbitFrom_.y) * 0.01f;
    window.pitch = std::clamp(window.pitch, -PI / 2.01f, PI / 2.01f);
    orbitFrom_ = now;
    return true;
  }

  const Vector2 mouse = GetMousePosition();

  if (dragging_) {
    if (dragView_ >= windows_.size()) { dragging_ = false; return false; }
    if (!dragMoved_ && Vector2Distance(mouse, dragGrab_) > 3.0f) dragMoved_ = true;
    if (dragMoved_) {
      dragScreenPos_ = {mouse.x - dragGrab_.x + dragScreenPos_.x,
                        mouse.y - dragGrab_.y + dragScreenPos_.y};
      dragGrab_ = mouse;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      SpaceWindow& window = windows_[dragView_];
      if (dragMoved_) {
        window.docked = false;
        window.worldPos = SnapToGrid(viewport.ToWorld(dragScreenPos_), viewport.Zoom());
      }
      dragging_ = false;
      dragMoved_ = false;
    }
    return true;
  }

  for (std::size_t i = 0; i < windows_.size(); ++i) {
    const Rectangle rect = RectFor(i, viewport, fonts);
    const bool      over = CheckCollisionPointRec(mouse, rect);
    if (!over && !PointOnRim(rect, mouse)) continue;

    // The wheel is this window's own, like every other window's.
    if (over) {
      const float wheel = GetMouseWheelMoveV().y;
      if (wheel != 0.0f) {
        windows_[i].dist = std::clamp(windows_[i].dist * (1.0f - wheel * 0.08f), 1.0f, 60.0f);
        return true;
      }
      // Right-drag orbits. A number line takes the right button to scroll its
      // own scale; this takes it to turn its own camera, for the same reason --
      // the left button is spoken for and the canvas can spare it.
      if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        orbiting_ = true;
        orbitView_ = i;
        orbitFrom_ = mouse;
        return true;
      }
    }
    if (PointOnRim(rect, mouse) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      dragging_ = true;
      dragMoved_ = false;
      dragView_ = i;
      dragGrab_ = mouse;
      dragScreenPos_ = {rect.x, rect.y};
      return true;
    }
    return over || PointOnRim(rect, mouse);
  }
  return false;
}

void SpacePanel::DrawScene(const SpaceWindow& window, Rectangle rect) {
  const int side = std::min(targetSide_, static_cast<int>(std::max(rect.width, rect.height)));
  if (side <= 1) return;

  Camera3D camera{};
  camera.target = {0.0f, 0.0f, 0.0f};
  camera.up = {0.0f, 1.0f, 0.0f};
  camera.fovy = 45.0f;
  camera.projection = CAMERA_PERSPECTIVE;
  camera.position = {0.0f, 0.0f, window.dist};
  camera.position = Vector3RotateByAxisAngle(camera.position, {1.0f, 0.0f, 0.0f}, window.pitch);
  camera.position = Vector3RotateByAxisAngle(camera.position, {0.0f, 1.0f, 0.0f}, window.yaw);

  const Vector3 limitsMin = {-kLimitX, -kLimitY, -kLimitZ};
  const Vector3 limitsMax = {kLimitX, kLimitY, kLimitZ};
  const float   cull = window.cullOutside ? 1.0f : 0.0f;
  const float   exemptOn = 1.0f;
  const float   exemptOff = 0.0f;

  SetShaderValue(solid_, solid_.locs[SHADER_LOC_VECTOR_VIEW], &camera.position,
                 SHADER_UNIFORM_VEC3);
  SetShaderValue(solid_, limitsMinLoc_, &limitsMin, SHADER_UNIFORM_VEC3);
  SetShaderValue(solid_, limitsMaxLoc_, &limitsMax, SHADER_UNIFORM_VEC3);
  SetShaderValue(solid_, cullLoc_, &cull, SHADER_UNIFORM_FLOAT);

  BeginTextureMode(target_);
  rlViewport(0, targetSide_ - side, side, side);
  ClearBackground(BLANK);
  BeginMode3D(camera);
  rlViewport(0, targetSide_ - side, side, side);

  // The sky first, with no depth written: it is behind everything by
  // construction rather than by being far away.
  skyBall_.materials[0].shader = sky_[window.kind == SpaceKind::Space ? 0 : 1];
  rlDisableDepthMask();
  DrawModel(skyBall_, {0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
  rlEnableDepthMask();

  // The cuboid: walls and floor, exempt from the cut they define.
  SetShaderValue(solid_, atInfinityLoc_, &exemptOn, SHADER_UNIFORM_FLOAT);
  DrawModelEx(wall_, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.0f,
              {kLimitX * 2.0f, kLimitY * 2.0f, kLimitZ * 2.0f}, Color{228, 228, 232, 255});
  DrawCubeWiresV({0.0f, 0.0f, 0.0f}, {kLimitX * 2.0f, kLimitY * 2.0f, kLimitZ * 2.0f}, BLACK);

  // And the points.
  for (std::size_t i = 0; i < window.refs.size(); ++i) {
    const PointRef& ref = window.refs[i];
    if (ref.block == nullptr) continue;
    for (const mathprogram::Variable& variable : ref.block->Variables()) {
      if (variable.name != ref.name) continue;
      const Point point = PointOf(variable.components);
      if (!point.known) break;
      const float exempt = point.atInfinity ? 1.0f : 0.0f;
      SetShaderValue(solid_, atInfinityLoc_, &exempt, SHADER_UNIFORM_FLOAT);
      const Color tint = VariableColor(static_cast<int>(i));
      DrawModel(ball_, point.at, kPointRadius * (point.atInfinity ? 6.0f : 1.0f), tint);
      break;
    }
  }

  EndMode3D();
  EndTextureMode();

  // Right side up: a render texture is bottom-up, and only its top-left corner
  // was drawn into.
  const Rectangle source = {0.0f, static_cast<float>(targetSide_ - side),
                            static_cast<float>(side), -static_cast<float>(side)};
  DrawTexturePro(target_.texture, source, rect, {0.0f, 0.0f}, 0.0f, WHITE);
}

void SpacePanel::Draw(const Viewport& viewport, FontStore& fonts) {
  if (windows_.empty()) return;
  EnsureGpu();

  const Vector2 mouse = GetMousePosition();
  for (std::size_t i = 0; i < windows_.size(); ++i) {
    const Rectangle rect = RectFor(i, viewport, fonts);
    if (rect.width < 2.0f || rect.height < 2.0f) continue;
    // Culled to the screen, like everything else: a window off the side of the
    // canvas costs nothing, and a 3D one would cost a great deal.
    if (rect.x > GetScreenWidth() || rect.y > GetScreenHeight()) continue;
    if (rect.x + rect.width < 0.0f || rect.y + rect.height < 0.0f) continue;

    DrawScene(windows_[i], rect);

    const bool hovered = CheckCollisionPointRec(mouse, rect) || PointOnRim(rect, mouse);
    if (hovered || windows_[i].selected) DrawRim(rect, viewport.Zoom(), hovered);
  }
}

// ------------------------------------------------------------------ selection
void SpacePanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                              FontStore& fonts) {
  if (lasso.size() < 3) return;
  for (std::size_t i = 0; i < windows_.size(); ++i) {
    windows_[i].selected = LassoCatches(lasso, RectFor(i, viewport, fonts));
  }
}

void SpacePanel::ClearSelection() {
  for (SpaceWindow& window : windows_) window.selected = false;
}

bool SpacePanel::AnySelected() const {
  for (const SpaceWindow& window : windows_) {
    if (window.selected) return true;
  }
  return false;
}

bool SpacePanel::SelectedRimAt(Vector2 point, const Viewport& viewport,
                               FontStore& fonts) const {
  for (std::size_t i = 0; i < windows_.size(); ++i) {
    if (!windows_[i].selected) continue;
    if (PointOnRim(RectFor(i, viewport, fonts), point)) return true;
  }
  return false;
}

void SpacePanel::BeginSelectionDrag() {
  for (SpaceWindow& window : windows_) window.dragFrom = window.worldPos;
}

void SpacePanel::MoveSelection(Vector2 worldDelta) {
  for (SpaceWindow& window : windows_) {
    if (!window.selected) continue;
    window.docked = false;
    window.worldPos = {window.dragFrom.x + worldDelta.x, window.dragFrom.y + worldDelta.y};
  }
}

void SpacePanel::DeleteSelection() {
  for (std::size_t i = windows_.size(); i-- > 0;) {
    if (!windows_[i].selected) continue;
    windows_.erase(windows_.begin() + static_cast<long>(i));
  }
}

void SpacePanel::DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const {
  for (std::size_t i = 0; i < windows_.size(); ++i) {
    if (!windows_[i].selected) continue;
    DrawRim(RectFor(i, viewport, fonts), viewport.Zoom(), true);
  }
}

void SpacePanel::Clear() {
  windows_.clear();
  dragging_ = false;
  orbiting_ = false;
  // A canvas with none of these should not be paying for one. The target and
  // the shaders come back by themselves the moment a window wants them.
  ReleaseGpu();
}

void SpacePanel::Adopt(std::vector<SpaceWindow> windows) { windows_ = std::move(windows); }

}  // namespace sb
