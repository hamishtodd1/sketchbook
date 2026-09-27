#include "spacedw.h"

#include <algorithm>
#include <cmath>

#include "grid.h"
#include "raymath.h"
#include "rlgl.h"
#include "selection.h"
#include "carry.h"
#include "hover.h"
#include "theme.h"

namespace sb {
namespace {

// How big one of these is on the canvas, and how far it sits from its box.

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

Point PointOf(const float* c) {
  Point p;
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

Point PointOf(const std::vector<float>& c) {
  if (c.size() < 4) return {};
  return PointOf(c.data());
}

// The one type these windows show. A trivector of a four-generator algebra is a
// point, and a point is what there is to draw here.
constexpr const char* kPointType = "Tri";

// Whether a box has a function whose input or answer is a point. Asked of every
// function it defines rather than only the one being read, so the window stays
// put as the caret moves.
bool WantsPointSamples(const CodeBlock& block) {
  for (const std::string& type : block.SampledTypes()) {
    if (type == kPointType) return true;
  }
  return false;
}

// One end of a fed function, as a point in the space -- or nothing, if that end
// is not a point at all.
bool SamplePoint(const Sampling& fed, int index, bool output, Point* out) {
  const std::string&        type = output ? fed.outputType : fed.inputType;
  const int                 stride = output ? fed.outStride : fed.inStride;
  const std::vector<float>& run = output ? fed.outputs : fed.inputs;
  if (type != kPointType || stride != 4 || index < 0) return false;
  if (static_cast<std::size_t>((index + 1) * 4) > run.size()) return false;
  *out = PointOf(run.data() + static_cast<std::size_t>(index) * 4);
  return out->known;
}

// How near counts as on a sample.
constexpr float kSampleGrab = 11.0f;

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
  glext::Release(&msaa_);
  haveGpu_ = false;
  targetWidth_ = 0;
  targetHeight_ = 0;
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
  targetWidth_ = 1024;
  targetHeight_ = 576;
  target_ = LoadRenderTexture(targetWidth_, targetHeight_);
  // Drawn into multisampled and resolved into the texture above, so the scene
  // is as smooth as everything drawn straight onto the window. Four samples,
  // which is what the window asks for. Without one, it draws single-sampled.
  glext::MakeMultisample(targetWidth_, targetHeight_, 4, &msaa_);
  haveGpu_ = true;
}

int SpacePanel::FlankCount(const CodeBlock* block) const {
  int many = 0;
  for (const SpaceWindow& window : windows_) {
    if (window.docked && window.dock == block) ++many;
  }
  return many;
}

Rectangle SpacePanel::RectFor(std::size_t index, const Viewport& viewport,
                              FontStore& fonts) const {
  const float        zoom = viewport.Zoom();
  const SpaceWindow& window = windows_[index];
  const float        tall = kFlankSide * zoom;
  const float        wide = tall * kSpaceAspect;

  if (dragging_ && dragMoved_ && index == dragView_) {
    return {dragScreenPos_.x, dragScreenPos_.y, wide, tall};
  }
  if (!window.docked || window.dock == nullptr) {
    const Vector2 topLeft = viewport.ToScreen(window.worldPos);
    return {topLeft.x, topLeft.y, wide, tall};
  }
  int slot = FlankBase(before_, window.dock);
  for (std::size_t i = 0; i < index; ++i) {
    if (windows_[i].docked && windows_[i].dock == window.dock) ++slot;
  }
  return FlankRect(window.dock->Measure(viewport, fonts).rect, slot, zoom, kSpaceAspect);
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
  // Only windows coming and going is a change: that is what moves boxes about.
  bool changed = false;

  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    bool            found = false;
    const SpaceKind kind = KindOfSignature(block->Algebra(), &found);

    // Every point the box declares, in the order it declares them. None, if the
    // box is not in an algebra that has points.
    std::vector<std::string> points;
    if (found) {
      for (const mathprogram::Variable& variable : block->Variables()) {
        if (variable.kind != mathprogram::Kind::Multivector) continue;
        if (variable.type != kPointType) continue;
        points.push_back(variable.name);
      }
    }
    const auto declared = [&](const std::string& name) {
      return std::find(points.begin(), points.end(), name) != points.end();
    };

    // A point that has gone leaves whichever window it was in -- and it could be
    // in any of them, since a point can be pulled out or merged elsewhere.
    for (SpaceWindow& window : windows_) {
      window.refs.erase(std::remove_if(window.refs.begin(), window.refs.end(),
                                       [&](const PointRef& ref) {
                                         return ref.block == block.get() && !declared(ref.name);
                                       }),
                        window.refs.end());
      if (found && window.dock == block.get()) window.kind = kind;
    }

    std::size_t home = windows_.size();
    for (std::size_t i = 0; i < windows_.size(); ++i) {
      if (windows_[i].home && windows_[i].dock == block.get()) { home = i; break; }
    }
    const auto ensureHome = [&]() {
      if (home < windows_.size()) return;
      SpaceWindow window;
      window.kind = kind;
      window.dock = block.get();
      windows_.push_back(std::move(window));
      home = windows_.size() - 1;
      changed = true;
    };

    // A point already in some window stays where it is. Only a point in no
    // window at all -- a new one -- goes to the box's own.
    for (const std::string& name : points) {
      bool present = false;
      for (const SpaceWindow& window : windows_) {
        for (const PointRef& ref : window.refs) {
          if (ref.block == block.get() && ref.name == name) { present = true; break; }
        }
        if (present) break;
      }
      if (present) continue;
      ensureHome();
      windows_[home].refs.push_back({block.get(), name});
    }
    // A box with no point of its own still gets a window if it has a function
    // that answers with one: the answers have to land somewhere.
    if (found && WantsPointSamples(*block)) ensureHome();
  }

  // A window goes when its box has, or when it has nothing left to show.
  for (std::size_t i = windows_.size(); i-- > 0;) {
    const SpaceWindow& window = windows_[i];
    bool alive = false;
    for (const std::unique_ptr<CodeBlock>& block : blocks) {
      if (block.get() == window.dock) { alive = true; break; }
    }
    const bool feeding = alive && window.home && WantsPointSamples(*window.dock);
    if (alive && (!window.refs.empty() || feeding)) continue;
    windows_.erase(windows_.begin() + static_cast<long>(i));
    // A window being held further along the list has just moved down one.
    if (dragging_ && dragView_ > i) --dragView_;
    else if (dragging_ && dragView_ == i) dragging_ = false;
    if (orbiting_ && orbitView_ > i) --orbitView_;
    else if (orbiting_ && orbitView_ == i) orbiting_ = false;
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
      // Dragging right turns the right-hand side towards you, which is a hand
      // on a globe. Negative because this camera is built by rotating (0,0,d)
      // about +y, where the surface and the volume put a -sin(yaw) in the eye
      // and so get there with a plus.
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
    // Dragging right turns the right-hand side towards you, the way a hand on a
    // globe does, and the same way round as every other window that turns.
    window.yaw -= (now.x - orbitFrom_.x) * 0.01f;
    window.pitch -= (now.y - orbitFrom_.y) * 0.01f;
    window.pitch = std::clamp(window.pitch, -PI / 2.01f, PI / 2.01f);
    orbitFrom_ = now;
    return true;
  }

  const Vector2 mouse = carry::Position();

  if (dragging_) {
    if (dragView_ >= windows_.size()) { dragging_ = false; return false; }
    if (!dragMoved_ && Vector2Distance(mouse, dragGrab_) > 3.0f) dragMoved_ = true;
    if (dragMoved_) {
      dragScreenPos_ = {mouse.x - dragGrab_.x + dragScreenPos_.x,
                        mouse.y - dragGrab_.y + dragScreenPos_.y};
      dragGrab_ = mouse;
    }
    if (carry::Released(MOUSE_BUTTON_LEFT)) {
      bool merged = false;
      // Dropped on another window of the same space: they share one from now
      // on, exactly as two number lines or two plots do -- a way of seeing the
      // points together, not a claim about them. A spacetime point has no
      // place in a space, so the two kinds never merge.
      for (std::size_t j = 0; dragMoved_ && j < windows_.size(); ++j) {
        if (j == dragView_ || windows_[j].kind != windows_[dragView_].kind) continue;
        if (!CheckCollisionPointRec(mouse, RectFor(j, viewport, fonts))) continue;
        for (const PointRef& ref : windows_[dragView_].refs) {
          bool already = false;
          for (const PointRef& have : windows_[j].refs) {
            if (have.block == ref.block && have.name == ref.name) { already = true; break; }
          }
          if (!already) windows_[j].refs.push_back(ref);
        }
        windows_.erase(windows_.begin() + static_cast<long>(dragView_));
        merged = true;
        break;
      }
      if (dragMoved_ && !merged) {
        SpaceWindow& window = windows_[dragView_];
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

namespace {

Camera3D CameraFor(const SpaceWindow& window) {
  Camera3D camera{};
  camera.target = {0.0f, 0.0f, 0.0f};
  camera.up = {0.0f, 1.0f, 0.0f};
  camera.fovy = 45.0f;
  camera.projection = CAMERA_PERSPECTIVE;
  camera.position = {0.0f, 0.0f, window.dist};
  camera.position = Vector3RotateByAxisAngle(camera.position, {1.0f, 0.0f, 0.0f}, window.pitch);
  camera.position = Vector3RotateByAxisAngle(camera.position, {0.0f, 1.0f, 0.0f}, window.yaw);
  return camera;
}

}  // namespace

bool SpacePanel::Project(std::size_t index, Vector3 at, const Viewport& viewport,
                         FontStore& fonts, Vector2* out) const {
  const Rectangle rect = RectFor(index, viewport, fonts);
  if (rect.width <= 1.0f || rect.height <= 1.0f) return false;
  // Projected back through the same shape the scene was drawn at.
  const Vector2 in = GetWorldToScreenEx(at, CameraFor(windows_[index]),
                                        static_cast<int>(rect.width),
                                        static_cast<int>(rect.height));
  *out = {rect.x + in.x, rect.y + in.y};
  return true;
}

bool SpacePanel::SampleMarker(const CodeBlock* block, int sample, bool output,
                              const Viewport& viewport, FontStore& fonts, Vector2* out) const {
  if (block == nullptr) return false;
  Point point{};
  if (!SamplePoint(block->Samples(), sample, output, &point)) return false;
  for (std::size_t i = 0; i < windows_.size(); ++i) {
    if (windows_[i].dock != block) continue;
    return Project(i, point.at, viewport, fonts, out);
  }
  return false;
}

bool SpacePanel::SampleUnder(Vector2 point, const Viewport& viewport, FontStore& fonts,
                             const CodeBlock** block, int* sample) const {
  float best = kSampleGrab * kSampleGrab;
  bool  found = false;
  for (std::size_t i = 0; i < windows_.size(); ++i) {
    const CodeBlock* dock = windows_[i].dock;
    if (!windows_[i].home || dock == nullptr || !dock->Samples().Live()) continue;
    const Rectangle rect = RectFor(i, viewport, fonts);
    if (!CheckCollisionPointRec(point, rect)) continue;
    const Sampling& fed = dock->Samples();
    for (int k = 0; k < fed.count; ++k) {
      for (int end = 0; end < 2; ++end) {
        Point where{};
        if (!SamplePoint(fed, k, end == 1, &where)) continue;
        Vector2 at{};
        if (!Project(i, where.at, viewport, fonts, &at)) continue;
        const float dx = at.x - point.x;
        const float dy = at.y - point.y;
        const float away = dx * dx + dy * dy;
        if (away >= best) continue;
        best = away;
        found = true;
        if (block != nullptr) *block = dock;
        if (sample != nullptr) *sample = k;
      }
    }
  }
  return found;
}

void SpacePanel::DrawScene(const SpaceWindow& window, Rectangle rect) {
  // Drawn at the window's size on screen, up to the size of the target -- a
  // window bigger than that is drawn at the target's size and stretched. The
  // target is the windows' shape, so the region drawn into is always a corner of
  // it with the same proportions, and the camera's aspect comes out right.
  const float fit = std::min(1.0f, static_cast<float>(targetWidth_) / std::max(rect.width, 1.0f));
  const int   wide = std::min(targetWidth_, static_cast<int>(rect.width * fit));
  const int   tall = std::min(targetHeight_, static_cast<int>(rect.height * fit));
  if (wide <= 1 || tall <= 1) return;

  const Camera3D camera = CameraFor(window);

  const Vector3 limitsMin = {-kLimitX, -kLimitY, -kLimitZ};
  const Vector3 limitsMax = {kLimitX, kLimitY, kLimitZ};
  const float   cull = window.cullOutside ? 1.0f : 0.0f;
  const float   exemptOn = 1.0f;

  SetShaderValue(solid_, solid_.locs[SHADER_LOC_VECTOR_VIEW], &camera.position,
                 SHADER_UNIFORM_VEC3);
  SetShaderValue(solid_, limitsMinLoc_, &limitsMin, SHADER_UNIFORM_VEC3);
  SetShaderValue(solid_, limitsMaxLoc_, &limitsMax, SHADER_UNIFORM_VEC3);
  SetShaderValue(solid_, cullLoc_, &cull, SHADER_UNIFORM_FLOAT);

  RenderTexture2D into = target_;
  if (msaa_.framebuffer != 0) into.id = msaa_.framebuffer;
  BeginTextureMode(into);
  rlViewport(0, targetHeight_ - tall, wide, tall);
  ClearBackground(BLANK);
  BeginMode3D(camera);
  rlViewport(0, targetHeight_ - tall, wide, tall);

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
      // The variable's own colour, which is the same wherever it is drawn. Its
      // index in this window used to be what chose it, and a point pulled out
      // into a window of its own would have changed colour on the way.
      const Color tint = ref.block->VariableColor(ref.name);
      DrawModel(ball_, point.at, kPointRadius * (point.atInfinity ? 6.0f : 1.0f), tint);
      break;
    }
  }

  // And the spread a function is being fed with, if this box is being read
  // inside one. A function has no one place to be, so this is what it looks
  // like: every sample put through it, drawn where its answer lands. Smaller
  // than a value's own marker, because a sample is a place the function was
  // asked about rather than a value anybody chose.
  if (window.home && window.dock != nullptr && window.dock->Samples().Live()) {
    const Sampling& fed = window.dock->Samples();
    const Color     tint = window.dock->VariableColor(fed.name);
    const int       lit = window.dock->SampleHover();
    for (int end = 0; end < 2; ++end) {
      for (int k = 0; k < fed.count; ++k) {
        Point where{};
        if (!SamplePoint(fed, k, end == 1, &where)) continue;
        const float exempt = where.atInfinity ? 1.0f : 0.0f;
        SetShaderValue(solid_, atInfinityLoc_, &exempt, SHADER_UNIFORM_FLOAT);
        const bool  on = (k == lit);
        const float size = kPointRadius * (on ? 1.1f : 0.65f) *
                           (where.atInfinity ? 6.0f : 1.0f);
        DrawModel(ball_, where.at, size, tint);
      }
    }
  }

  EndMode3D();
  EndTextureMode();
  if (msaa_.framebuffer != 0) glext::Resolve(msaa_, target_.id);

  // Right side up: a render texture is bottom-up, and only its top-left corner
  // was drawn into.
  const Rectangle source = {0.0f, static_cast<float>(targetHeight_ - tall),
                            static_cast<float>(wide), -static_cast<float>(tall)};
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

    const bool    carried = dragging_ && dragMoved_ && i == dragView_;
    const Vector2 aim = hover::Pointer();
    const bool    hovered = carried || CheckCollisionPointRec(aim, rect) || PointOnRim(rect, aim);
    if (hovered || windows_[i].selected) DrawRim(rect, viewport.Zoom(), hovered);
  }
}

// ------------------------------------------------------------------ selection
void SpacePanel::Rects(const Viewport& viewport, FontStore& fonts,
                       std::vector<Rectangle>* out) const {
  AppendRects(windows_, out, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void SpacePanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport,
                              FontStore& fonts) {
  if (lasso.size() < 3) return;
  SelectCaught(windows_, lasso, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void SpacePanel::ClearSelection() { ClearSelected(windows_); }

bool SpacePanel::AnySelected() const { return sb::AnySelected(windows_); }

bool SpacePanel::SelectedRimAt(Vector2 point, const Viewport& viewport, FontStore& fonts) const {
  return SelectedRim(windows_, point, [&](std::size_t i) { return RectFor(i, viewport, fonts); });
}

void SpacePanel::BeginSelectionDrag() { RememberPositions(windows_); }

void SpacePanel::MoveSelection(Vector2 worldDelta) {
  // Not MoveSelected: one of these carried out of a selection comes off its box
  // rather than staying docked to it, since a space window docks beside a box
  // rather than under it and there is nothing to follow.
  for (SpaceWindow& window : windows_) {
    if (!window.selected) continue;
    window.docked = false;
    window.worldPos = {window.dragFrom.x + worldDelta.x, window.dragFrom.y + worldDelta.y};
  }
}

void SpacePanel::DeleteSelection() { EraseSelected(windows_); }

void SpacePanel::DrawSelectionRims(const Viewport& viewport, FontStore& fonts) const {
  DrawSelectedRims(windows_, viewport.Zoom(),
                   [&](std::size_t i) { return RectFor(i, viewport, fonts); });
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

bool SpacePanel::VizUnder(Vector2 point, const Viewport& viewport, FontStore& fonts,
                          PointRef* out, Rectangle* window) const {
  constexpr float kGrab = 14.0f;   // screen pixels, like the rest of the chrome
  for (std::size_t i = windows_.size(); i-- > 0;) {
    const Rectangle rect = RectFor(i, viewport, fonts);
    if (!CheckCollisionPointRec(point, rect) || PointOnRim(rect, point)) continue;
    float best = kGrab * kGrab;
    bool  found = false;
    for (const PointRef& ref : windows_[i].refs) {
      if (ref.block == nullptr) continue;
      const mathprogram::Variable* variable = ref.block->FindVariable(ref.name);
      if (variable == nullptr) continue;
      const Point where = PointOf(variable->components);
      if (!where.known) continue;
      Vector2 at{};
      if (!Project(i, where.at, viewport, fonts, &at)) continue;
      const float away = (at.x - point.x) * (at.x - point.x) + (at.y - point.y) * (at.y - point.y);
      if (away >= best) continue;
      best = away;
      *out = ref;
      found = true;
    }
    if (found) *window = rect;
    // The window under the point is the one that answers, point or no point.
    return found;
  }
  return false;
}

bool SpacePanel::BeginPullOut(CodeBlock* block, const std::string& name, Vector2 pointer,
                              const Viewport& viewport) {
  if (block == nullptr || name.empty() || dragging_ || orbiting_) return false;
  std::size_t from = windows_.size();
  for (std::size_t i = 0; i < windows_.size() && from == windows_.size(); ++i) {
    for (const PointRef& ref : windows_[i].refs) {
      if (ref.block == block && ref.name == name) { from = i; break; }
    }
  }
  if (from == windows_.size()) return false;

  SpaceWindow made;
  const SpaceWindow& source = windows_[from];
  made.kind = source.kind;
  made.yaw = source.yaw;
  made.pitch = source.pitch;
  made.dist = source.dist;
  made.cullOutside = source.cullOutside;
  made.dock = block;
  made.docked = false;
  made.home = false;
  made.refs.push_back({block, name});

  std::vector<PointRef>& refs = windows_[from].refs;
  refs.erase(std::remove_if(refs.begin(), refs.end(),
                            [&](const PointRef& ref) {
                              return ref.block == block && ref.name == name;
                            }),
             refs.end());
  // The window it left goes if that was all it had -- now rather than at the
  // next Sync, so nothing is erased from under the carry that is about to begin.
  const bool feeding = windows_[from].home && windows_[from].dock != nullptr &&
                       WantsPointSamples(*windows_[from].dock);
  if (refs.empty() && !feeding) windows_.erase(windows_.begin() + static_cast<long>(from));

  windows_.push_back(std::move(made));
  const float tall = kFlankSide * viewport.Zoom();
  const float wide = tall * kSpaceAspect;
  dragging_ = true;
  dragMoved_ = true;
  dragView_ = windows_.size() - 1;
  dragGrab_ = pointer;
  // Carried from its middle, so it arrives under the pointer.
  dragScreenPos_ = {pointer.x - wide * 0.5f, pointer.y - tall * 0.5f};
  return true;
}

}  // namespace sb
