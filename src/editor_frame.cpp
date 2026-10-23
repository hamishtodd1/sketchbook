// The editor's frames: the video drawn -- canvas, then face -- into a target of
// its own, for the monitor or for the render; and the render itself.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "editor.h"
#include "raylib.h"
#include "rlgl.h"
#include "screen.h"
#include "theme.h"

namespace sb {
namespace {

// The face: a rounded rectangle, its edge as coverage like every other edge.
const char* kFaceVertex = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragPos;
out vec2 fragTexCoord;
out vec4 fragColor;
void main() {
  fragPos = vertexPosition.xy;
  fragTexCoord = vertexTexCoord;
  fragColor = vertexColor;
  gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

const char* kFaceFragment = R"(#version 330
in vec2 fragPos;
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 rect;      // x, y, width, height, in the frame's pixels
uniform float radius;
out vec4 finalColor;
void main() {
  vec2  half = rect.zw * 0.5;
  vec2  q = abs(fragPos - (rect.xy + half)) - half + vec2(radius);
  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
  vec4  c = texture(texture0, fragTexCoord);
  finalColor = vec4(c.rgb, clamp(0.5 - d, 0.0, 1.0)) * fragColor;
}
)";

constexpr float kFaceHeight = 0.28f;   // of the frame's height
constexpr float kFaceMargin = 0.035f;

std::vector<float> Mix(const std::vector<float>& a, const std::vector<float>& b, float s) {
  std::vector<float> out(std::min(a.size(), b.size()));
  const float e = s * s * s * (s * (s * 6.0f - 15.0f) + 10.0f);
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = a[i] + (b[i] - a[i]) * e;
  return out;
}

}  // namespace

// ------------------------------------------------------------------ targets

void Editor::EnsureTarget(Target* target, int width, int height) {
  width = std::max(width, 16);
  height = std::max(height, 16);
  if (target->width == width && target->height == height && target->resolved.id != 0) return;
  ReleaseTarget(target);
  if (!glext::MakeMultisample(width, height, 4, &target->msaa)) target->msaa = {};
  target->resolved = LoadRenderTexture(width, height);
  SetTextureFilter(target->resolved.texture, TEXTURE_FILTER_BILINEAR);
  target->width = width;
  target->height = height;
}

void Editor::ReleaseTarget(Target* target) {
  glext::Release(&target->msaa);
  if (target->resolved.id != 0) UnloadRenderTexture(target->resolved);
  *target = {};
}

// ------------------------------------------------------------------ the face

std::vector<float> Editor::DefaultFace() const {
  // Bottom right, a little in from the edges, in heights of the frame.
  const float aspect = faceInfo_.height > 0
                           ? static_cast<float>(faceInfo_.width) / static_cast<float>(faceInfo_.height)
                           : 16.0f / 9.0f;
  const float wide = static_cast<float>(kVideoWidth) / static_cast<float>(kVideoHeight);
  return {wide - kFaceMargin - kFaceHeight * aspect * 0.5f, 1.0f - kFaceMargin - kFaceHeight * 0.5f,
          kFaceHeight};
}

std::vector<float> Editor::FaceAt(double source) const {
  const edit::Track* track = edit_.FindTrack("face");
  return track != nullptr ? edit::Evaluate(*track, source, DefaultFace()) : DefaultFace();
}

Rectangle Editor::FaceRect(const std::vector<float>& face, Rectangle frame) const {
  if (face.size() < 3) return {};
  const float aspect = faceInfo_.height > 0
                           ? static_cast<float>(faceInfo_.width) / static_cast<float>(faceInfo_.height)
                           : 16.0f / 9.0f;
  const float h = face[2] * frame.height;
  const float w = h * aspect;
  return {frame.x + face[0] * frame.height - w * 0.5f, frame.y + face[1] * frame.height - h * 0.5f,
          w, h};
}

void Editor::FaceFrame(double cameraTime, bool wait) {
  if (faceStream_ == nullptr) return;
  cameraTime = std::max(0.0, cameraTime);
  // A jump back, or a long way on, is a seek; anything else is the decoder
  // carrying on.
  if (faceAsked_ < 0.0 || cameraTime < faceAsked_ - 0.02 || cameraTime > faceAsked_ + 1.5) {
    faceStream_->Seek(cameraTime);
  }
  faceAsked_ = cameraTime;
  bool changed = faceStream_->Advance(cameraTime);
  if (wait) {
    // A render is not in real time: it waits for the very frame.
    const double frame = 1.0 / std::max(1.0, faceInfo_.fps);
    const auto   giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (faceStream_->CurrentTime() < cameraTime - frame - 1e-6 && !faceStream_->Ended() &&
           std::chrono::steady_clock::now() < giveUp) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      changed = faceStream_->Advance(cameraTime) || changed;
    }
  }
  if (changed && faceStream_->HaveFrame()) UpdateTexture(faceTexture_, faceStream_->Pixels().data());
}

// ------------------------------------------------------------------ a frame

void Editor::Compose(App& app, Target& target, bool waitForFace) {
  RenderTexture2D into = target.resolved;
  if (target.msaa.framebuffer != 0) into.id = target.msaa.framebuffer;
  OuterTarget() = &into;
  BeginTextureMode(into);
  app.Draw();

  // The face, where its keys put it. Across a transition it goes from where it
  // was to where it will be, still at the frame either side.
  if (faceStream_ != nullptr && faceTexture_.id != 0) {
    const edit::Moment moment = edit_.At(playhead_);
    std::vector<float> face;
    double             source = moment.source;
    if (moment.crossing) {
      const edit::Piece& before = edit_.Pieces()[static_cast<std::size_t>(moment.piece - 1)];
      const edit::Piece& after = edit_.Pieces()[static_cast<std::size_t>(moment.piece)];
      face = Mix(FaceAt(before.to), FaceAt(after.from), static_cast<float>(moment.through));
      source = moment.through < 0.5 ? before.to : after.from;
    } else {
      face = FaceAt(source);
    }
    const Rectangle frame = {0.0f, 0.0f, static_cast<float>(target.width),
                             static_cast<float>(target.height)};
    const Rectangle r = FaceRect(face, frame);
    if (r.height > 1.0f) {
      FaceFrame(source + cameraOffset_, waitForFace);
      const float radius = r.height * 0.08f;
      DrawRectangleRounded({r.x - 2.0f, r.y + 1.0f, r.width + 4.0f, r.height + 4.0f},
                           radius * 2.0f / std::min(r.width, r.height), 8, Fade(BLACK, 0.18f));
      BeginShaderMode(faceShader_);
      const float rect[4] = {r.x, r.y, r.width, r.height};
      SetShaderValue(faceShader_, faceRectLoc_, rect, SHADER_UNIFORM_VEC4);
      SetShaderValue(faceShader_, faceRadiusLoc_, &radius, SHADER_UNIFORM_FLOAT);
      DrawTexturePro(faceTexture_,
                     {0.0f, 0.0f, static_cast<float>(faceTexture_.width),
                      static_cast<float>(faceTexture_.height)},
                     r, {0.0f, 0.0f}, 0.0f, WHITE);
      EndShaderMode();
    }
  }

  // The pointer, where the recording had it, wherever its switch is on.
  if (pointerKnown_ && edit_.On("pointer", edit_.SourceNear(playhead_))) DrawPointer(target);

  EndTextureMode();
  OuterTarget() = nullptr;
  if (target.msaa.framebuffer != 0) glext::Resolve(target.msaa, target.resolved.id);
}

void Editor::DrawPointer(const Target& target) {
  if (!pointerFill_.Ready() && !pointerFill_.Load()) return;
  const Vector2 tip = app_->ToScreen(pointer_);
  // The arrow everybody knows, at a size that reads in a video.
  const float          scale = 1.3f * static_cast<float>(target.height) / 1080.0f;
  const morph::Point   arrow[] = {{0, 0},  {0, 21},  {5, 16}, {9, 25},
                                  {12, 24}, {8, 15}, {15, 15}};
  morph::Contour       outline;
  for (const morph::Point& p : arrow) outline.push_back({tip.x + p.x * scale, tip.y + p.y * scale});
  pointerFill_.Draw({{{outline}, 1.0f}}, WHITE);
  for (std::size_t i = 0; i < outline.size(); ++i) {
    const morph::Point& a = outline[i];
    const morph::Point& b = outline[(i + 1) % outline.size()];
    DrawLineEx({a.x, a.y}, {b.x, b.y}, 1.4f * scale, Fade(BLACK, 0.85f));
  }
}

void Editor::DrawFrame(App& app) {
  if (render_.on) {
    Render& r = render_;
    Compose(app, r.target, /*waitForFace=*/true);
    rlEnableFramebuffer(r.target.resolved.id);
    glext::ReadPixels(0, 0, kVideoWidth, kVideoHeight, r.pixels.data());
    rlDisableFramebuffer();
    if (!r.encoder.Write(r.pixels.data(), r.pixels.size())) {
      StopRender(true);   // ffmpeg went: say what it said
    } else if (++r.frame >= r.frames) {
      StopRender(true);
    }
  } else if (monitor_.resolved.id != 0) {
    Compose(app, monitor_, /*waitForFace=*/false);
  }
  SetScreenOverride(0, 0);
}

// ------------------------------------------------------------------ rendering

void Editor::StartRender() {
  if (render_.on) return;
  const std::string& ffmpeg = video::ToolPath();
  if (ffmpeg.empty()) {
    note_ = "No ffmpeg on the path, so nothing to render with";
    noteUntil_ = GetTime() + 6.0;
    return;
  }
  Render& r = render_;
  EnsureTarget(&r.target, kVideoWidth, kVideoHeight);
  r.pixels.assign(static_cast<std::size_t>(kVideoWidth) * kVideoHeight * 4, 0);
  const std::filesystem::path folder(folder_);
  r.part = (folder / "video.part.mp4").string();
  r.done = (folder / "video.mp4").string();
  // Raw frames in, bottom row first as GL reads them; an H.264 file out, of the
  // kind anything will play.
  const std::string size = std::to_string(kVideoWidth) + "x" + std::to_string(kVideoHeight);
  std::string command =
      proc::Quote(ffmpeg) + " -y -hide_banner -loglevel error -f rawvideo -pix_fmt rgba -s " +
      size + " -r " + std::to_string(static_cast<int>(kVideoRate)) + " -i -";
  if (cameraSound_ || !edit_.Sounds().empty()) {
    // The microphone, cut as the pieces are, and the sounds laid over it. The
    // graph goes in a file: a long edit's would not fit on a command line.
    if (cameraSound_) command += " -i " + proc::Quote(camera_);
    for (const edit::Sound& sound : edit_.Sounds()) {
      command += " -i " + proc::Quote((folder / sound.file).string());
    }
    const std::string graphPath = (folder / "render.filter").string();
    if (std::FILE* file = std::fopen(graphPath.c_str(), "wb")) {
      const std::string graph = SoundGraph(cameraSound_ ? 2 : 1);
      std::fwrite(graph.data(), 1, graph.size(), file);
      std::fclose(file);
    }
    command += " -/filter_complex " + proc::Quote(graphPath) +
               " -map [picture] -map [sound] -c:a aac -b:a 192k";
  } else {
    command += " -vf vflip";
  }
  command += " -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -movflags +faststart " +
             proc::Quote(r.part);
  if (!r.encoder.Start(command, (folder / "render.log").string())) {
    note_ = "ffmpeg would not start";
    noteUntil_ = GetTime() + 6.0;
    StopRender(false);
    return;
  }
  r.on = true;
  if (soundPlaying_) PauseMusicStream(sound_);
  soundPlaying_ = false;
  r.frame = 0;
  r.frames = static_cast<int>(std::ceil(edit_.Duration() * kVideoRate));
  playing_ = false;
  scrubbing_ = false;
  faceAsked_ = -1.0;
}

void Editor::StopRender(bool finished) {
  Render& r = render_;
  SetScreenOverride(0, 0);
  if (finished) {
    const int  code = r.encoder.Finish();
    const bool moved = code == 0 && edit::MoveOver(r.part, r.done);
    note_ = moved ? "Rendered to " + r.done : "The render failed; render.log says why";
  } else {
    r.encoder.Stop();
    std::error_code ec;
    std::filesystem::remove(r.part, ec);
    if (note_.empty() || GetTime() > noteUntil_) note_ = "Render stopped";
  }
  noteUntil_ = GetTime() + 8.0;
  ReleaseTarget(&r.target);
  r.pixels.clear();
  r.on = false;
  faceAsked_ = -1.0;
  TraceLog(LOG_INFO, "EDIT: %s", note_.c_str());
}

// ------------------------------------------------------------------ opening

void Editor::OpenFace() {
  if (camera_.empty()) return;
  faceInfo_ = video::Probe(camera_);
  if (!faceInfo_.ok || faceInfo_.width <= 0 || faceInfo_.height <= 0) return;
  faceStream_ = std::make_unique<video::Stream>(camera_, faceInfo_, 640);
  faceTexture_.id = rlLoadTexture(nullptr, faceStream_->Width(), faceStream_->Height(),
                                  PIXELFORMAT_UNCOMPRESSED_R8G8B8, 1);
  faceTexture_.width = faceStream_->Width();
  faceTexture_.height = faceStream_->Height();
  faceTexture_.mipmaps = 1;
  faceTexture_.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8;
  SetTextureFilter(faceTexture_, TEXTURE_FILTER_BILINEAR);
  faceShader_ = LoadShaderFromMemory(kFaceVertex, kFaceFragment);
  faceRectLoc_ = GetShaderLocation(faceShader_, "rect");
  faceRadiusLoc_ = GetShaderLocation(faceShader_, "radius");
}

void Editor::CloseFace() {
  pointerFill_.Unload();
  faceStream_.reset();
  if (faceTexture_.id != 0) UnloadTexture(faceTexture_);
  faceTexture_ = {};
  if (faceShader_.id != 0) UnloadShader(faceShader_);
  faceShader_ = {};
  ReleaseTarget(&monitor_);
}

}  // namespace sb
