#include "editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "capture.h"
#include "filedialog.h"
#include "grip.h"
#include "raylib.h"
#include "screen.h"
#include "text.h"
#include "theme.h"

namespace sb {
namespace {

constexpr float kPanelHeight = 148.0f;   // screen pixels, like all chrome
constexpr float kInset = 24.0f;
constexpr float kPieceGap = 3.0f;        // between two pieces, where a cut is
constexpr float kLaneHeight = 12.0f;     // a row of keys
constexpr float kSwitchLane = 18.0f;     // a switch's timeline
constexpr float kSoundLane = 22.0f;      // a sound's
constexpr Color kWindowBg = {22, 22, 20, 255};
constexpr Color kTimelineBg = {30, 31, 27, 255};
constexpr Color kPieceFill = {74, 96, 130, 255};
constexpr Color kPieceChosen = {108, 158, 230, 255};
constexpr Color kPlayhead = {236, 98, 92, 255};
constexpr Color kJoinFill = {150, 118, 196, 255};
constexpr Color kJoinChosen = {196, 160, 240, 255};
constexpr Color kCameraKey = {236, 196, 92, 255};
constexpr Color kFaceKey = {120, 206, 140, 255};
constexpr Color kBoxKey = {120, 176, 236, 255};
constexpr Color kSpanFill = {196, 150, 92, 255};
constexpr Color kSpanChosen = {236, 196, 140, 255};
constexpr Color kSoundFill = {82, 150, 140, 255};
constexpr Color kSoundChosen = {130, 206, 190, 255};

// ffprobe, beside ffmpeg.
std::string Ffprobe() {
  const std::string& ffmpeg = video::ToolPath();
  const std::size_t  at = ffmpeg.rfind("ffmpeg");
  if (at == std::string::npos) return "ffprobe";
  return ffmpeg.substr(0, at) + "ffprobe" + ffmpeg.substr(at + 6);
}
constexpr Color kTick = {150, 150, 140, 255};
constexpr double kMaxPxPerSecond = 900.0;   // a frame is fifteen pixels
constexpr std::size_t kUndoSteps = 200;

std::string Clock(double seconds) {
  // In tenths, rounded: 1.4 written down as 1.39999 is still 1.4.
  const long tenths = std::lround(std::max(seconds, 0.0) * 10.0);
  char out[32];
  std::snprintf(out, sizeof(out), "%ld:%02ld.%ld", tenths / 600, (tenths / 10) % 60, tenths % 10);
  return out;
}

// Which lane a track's keys are drawn in: the camera, the face, or the boxes.
int LaneOf(const std::string& target) {
  if (target == "camera") return 0;
  if (target == "face") return 1;
  return target.rfind("box:", 0) == 0 ? 2 : -1;
}

}  // namespace

bool Editor::Open(const std::string& folder, App* app, std::string* error) {
  if (!recording_.Load(folder, error)) return false;
  if (!edit_.Load(folder, recording_.Duration(), error)) return false;
  folder_ = folder;
  app_ = app;
  app_->BeginPlayback(recording_.CanvasPath());
  before_ = edit_;

  camera_ = capture::CameraFile(folder);
  double cameraStart = 0.0;
  if (!camera_.empty() && recording_.WallClock() > 0.0 &&
      capture::CameraStart(folder, &cameraStart)) {
    cameraOffset_ = recording_.WallClock() - cameraStart;
  }
  const std::string& ffmpeg = video::ToolPath();
  if (!camera_.empty() && !ffmpeg.empty()) {
    const std::string ffprobe = ffmpeg.substr(0, ffmpeg.rfind("ffmpeg")) + "ffprobe" +
                                ffmpeg.substr(ffmpeg.rfind("ffmpeg") + 6);
    cameraSound_ = !proc::Capture(proc::Quote(ffprobe) +
                                  " -v error -select_streams a -show_entries stream=index"
                                  " -of csv=p=0 " + proc::Quote(camera_)).empty();
  }
  OpenFace();
  if (cameraSound_) {
    // Made off the frame loop: an hour of it takes a while.
    const std::filesystem::path preview = std::filesystem::path(folder) / "preview.mp3";
    if (std::filesystem::exists(preview)) {
      previewMade_ = true;
    } else {
      const std::string part = preview.string() + ".part.mp3";
      const std::string done = preview.string();
      const std::string command = proc::Quote(ffmpeg) + " -y -v error -i " + proc::Quote(camera_) +
                                  " -vn -af aresample=async=1000:first_pts=0 -ac 1 -ar 44100"
                                  " -b:a 96k " + proc::Quote(part);
      previewMaker_ = std::thread([this, command, part, done] {
        proc::Capture(command);
        previewMade_ = edit::MoveOver(part, done);
      });
    }
  }
  playhead_ = 0.0;
  shownFrame_ = -2;
  pxPerSecond_ = 0.0;
  return true;
}

void Editor::Close() {
  if (previewMaker_.joinable()) previewMaker_.join();
  for (Player& player : players_) {
    if (player.loaded) UnloadMusicStream(player.music);
  }
  players_.clear();
  if (soundLoaded_) {
    StopMusicStream(sound_);
    UnloadMusicStream(sound_);
    soundLoaded_ = false;
  }
  if (render_.on) StopRender(false);
  CloseFace();
}

// ------------------------------------------------------------------ sound

void Editor::PlaySound(const edit::Moment& moment) {
  if (!soundLoaded_ && previewMade_ && IsAudioDeviceReady()) {
    if (previewMaker_.joinable()) previewMaker_.join();
    const std::string preview = (std::filesystem::path(folder_) / "preview.mp3").string();
    sound_ = LoadMusicStream(preview.c_str());
    sound_.looping = false;
    soundLoaded_ = IsMusicValid(sound_);
  }
  if (!soundLoaded_) return;
  const double at = moment.source + cameraOffset_;
  const bool   wanted = playing_ && !moment.crossing && at >= 0.0 &&
                      at < GetMusicTimeLength(sound_);
  if (!wanted) {
    if (soundPlaying_) PauseMusicStream(sound_);
    soundPlaying_ = false;
    return;
  }
  if (!soundPlaying_) {
    PlayMusicStream(sound_);
    SeekMusicStream(sound_, static_cast<float>(at));
    soundPlaying_ = true;
  } else if (std::abs(GetMusicTimePlayed(sound_) - at) > 0.25) {
    // A cut, or drift: put it back where the picture is.
    SeekMusicStream(sound_, static_cast<float>(at));
  }
  UpdateMusicStream(sound_);
}

std::string Editor::SoundGraph(int firstLaid) const {
  // Every part the same shape, so they can be joined end to end.
  const char* shape = "aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo";
  std::string graph;
  std::string joined;
  int         parts = 0;
  const auto  silence = [&](double seconds) {
    if (seconds <= 0.0) return;
    const std::string name = "[s" + std::to_string(parts) + "]";
    graph += "anullsrc=r=48000:cl=stereo,atrim=duration=" + std::to_string(seconds) + "," +
             shape + name + ";";
    joined += name;
    ++parts;
  };
  for (int i = 0; i < static_cast<int>(edit_.Pieces().size()); ++i) {
    silence(edit_.Transition(i));
    const edit::Piece& piece = edit_.Pieces()[static_cast<std::size_t>(i)];
    const double from = piece.from + cameraOffset_;
    const double to = piece.to + cameraOffset_;
    // Before the camera began, there is nothing to hear.
    const double lead = std::min(std::max(0.0, -from), piece.Length());
    silence(lead);
    if (to > 0.0 && piece.Length() - lead > 0.0) {
      const std::string name = "[p" + std::to_string(parts) + "]";
      // Held to its timestamps first: a microphone's clock and the wall's drift
      // apart over an hour, and the wall's is the one the session keeps.
      graph += "[1:a]aresample=async=1000:first_pts=0,atrim=start=" +
               std::to_string(std::max(0.0, from)) + ":end=" + std::to_string(to) +
               ",asetpts=PTS-STARTPTS," + shape + ",apad,atrim=duration=" +
               std::to_string(piece.Length() - lead) + name + ";";
      joined += name;
      ++parts;
    }
  }
  // The microphone's sound, cut as the pieces are -- or, with no camera, the
  // silence the laid sounds go over.
  if (cameraSound_ && parts > 0) {
    graph += joined + "concat=n=" + std::to_string(parts) + ":v=0:a=1[voice];";
  } else {
    graph = "anullsrc=r=48000:cl=stereo,atrim=duration=" + std::to_string(edit_.Duration()) +
            "," + shape + "[voice];";
  }
  // Each laid sound: its stretch of its file, as loud as it was set, put where
  // it goes; all of them mixed over the voice, for as long as the voice lasts.
  std::string mixed = "[voice]";
  const std::vector<edit::Sound>& sounds = edit_.Sounds();
  for (std::size_t j = 0; j < sounds.size(); ++j) {
    const edit::Sound& sound = sounds[j];
    const std::string  name = "[laid" + std::to_string(j) + "]";
    graph += "[" + std::to_string(firstLaid + static_cast<int>(j)) + ":a]atrim=start=" +
             std::to_string(sound.from) + ":end=" + std::to_string(sound.to) +
             ",asetpts=PTS-STARTPTS," + shape + ",volume=" + std::to_string(sound.gain) +
             ",adelay=delays=" + std::to_string(static_cast<long>(sound.at * 1000.0)) +
             ":all=1" + name + ";";
    mixed += name;
  }
  if (sounds.empty()) {
    graph += "[voice]anull[sound];";
  } else {
    graph += mixed + "amix=inputs=" + std::to_string(sounds.size() + 1) +
             ":duration=first:normalize=0[sound];";
  }
  return graph + "[0:v]vflip[picture]";
}

// ------------------------------------------------------------------ geometry

Rectangle Editor::Panel() const {
  const float w = static_cast<float>(GetScreenWidth());
  const float h = static_cast<float>(GetScreenHeight());
  const float tall = kPanelHeight + kSwitchLane * static_cast<float>(Ticked().size()) +
                     kSoundLane * static_cast<float>(edit_.Sounds().size());
  return {0.0f, h - tall, w, tall};
}

std::vector<std::string> Editor::Ticked() const {
  std::vector<std::string> out;
  for (const edit::SwitchKind& kind : edit::kSwitchKinds) {
    const edit::Switch* s = edit_.FindSwitch(kind.name);
    if (s != nullptr && s->ticked) out.push_back(kind.name);
  }
  return out;
}

Rectangle Editor::SwitchRow(int lane) const {
  const Rectangle row = KeyRow();
  return {row.x, row.y + row.height + 4.0f + kSwitchLane * static_cast<float>(lane), row.width,
          kSwitchLane - 3.0f};
}

Rectangle Editor::SpanRect(int lane, const edit::Span& span) const {
  const Rectangle row = SwitchRow(lane);
  const float     x0 = XAt(edit_.OutputNear(span.from, true));
  const float     x1 = XAt(edit_.OutputNear(span.to, false));
  return {x0, row.y, std::max(x1 - x0, 2.0f), row.height};
}

Rectangle Editor::SoundRow(int index) const {
  const Rectangle row = KeyRow();
  const float     below = row.y + row.height + 4.0f + kSwitchLane * static_cast<float>(Ticked().size());
  return {row.x, below + kSoundLane * static_cast<float>(index), row.width, kSoundLane - 3.0f};
}

Rectangle Editor::SoundRect(int index) const {
  const edit::Sound& sound = edit_.Sounds()[static_cast<std::size_t>(index)];
  const Rectangle    row = SoundRow(index);
  const float        x0 = XAt(sound.at);
  const float        x1 = XAt(sound.at + sound.Length());
  return {x0, row.y, std::max(x1 - x0, 2.0f), row.height};
}

Rectangle Editor::SettingsButton() const {
  const Rectangle panel = Panel();
  return {panel.x + panel.width - kInset - 72.0f, panel.y + panel.height - 22.0f, 72.0f, 18.0f};
}

Rectangle Editor::SettingsRow(int index) const {
  const Rectangle button = SettingsButton();
  constexpr float kRow = 26.0f;
  const int       rows = static_cast<int>(std::size(edit::kSwitchKinds)) + 1;
  return {button.x + button.width - 220.0f,
          button.y - 10.0f - kRow * static_cast<float>(rows - index), 220.0f, kRow};
}

Rectangle Editor::Ruler() const {
  const Rectangle panel = Panel();
  return {panel.x + kInset, panel.y + 8.0f, panel.width - 2.0f * kInset, 24.0f};
}

Rectangle Editor::Track() const {
  const Rectangle ruler = Ruler();
  return {ruler.x, ruler.y + ruler.height + 6.0f, ruler.width, 40.0f};
}

Rectangle Editor::KeyRow() const {
  const Rectangle track = Track();
  return {track.x, track.y + track.height + 6.0f, track.width, 3.0f * kLaneHeight};
}

Rectangle Editor::Monitor() const {
  // The video's shape, as big as fits above the timeline.
  const float w = static_cast<float>(GetScreenWidth()) - 2.0f * kInset;
  const float h = Panel().y - 2.0f * 12.0f;
  const float scale = std::max(0.01f, std::min(w / kVideoWidth, h / kVideoHeight));
  const float mw = std::floor(kVideoWidth * scale);
  const float mh = std::floor(kVideoHeight * scale);
  return {std::floor((static_cast<float>(GetScreenWidth()) - mw) * 0.5f),
          std::floor((Panel().y - mh) * 0.5f), mw, mh};
}

void Editor::Fit() {
  viewStart_ = 0.0;
  pxPerSecond_ = Track().width / std::max(edit_.Duration(), 1e-3);
}

double Editor::TimeAt(float x) const { return viewStart_ + (x - Track().x) / pxPerSecond_; }

float Editor::XAt(double time) const {
  return Track().x + static_cast<float>((time - viewStart_) * pxPerSecond_);
}

Rectangle Editor::PieceRect(int index) const {
  const Rectangle track = Track();
  const double    start = edit_.Start(index);
  const double    length = edit_.Pieces()[static_cast<std::size_t>(index)].Length();
  float           x0 = XAt(start), x1 = XAt(start + length);
  // A gap where one piece meets the next, so a cut can be seen.
  if (index > 0) x0 += kPieceGap * 0.5f;
  if (index + 1 < static_cast<int>(edit_.Pieces().size())) x1 -= kPieceGap * 0.5f;
  return {x0, track.y, std::max(x1 - x0, 1.0f), track.height};
}

Rectangle Editor::JoinRect(int index) const {
  if (index <= 0 || index >= static_cast<int>(edit_.Pieces().size())) return {};
  const double length = edit_.Transition(index);
  const double end = edit_.Start(index);
  const Rectangle track = Track();
  // A cut that snaps still shows where it is: a sliver to click.
  const float x0 = XAt(end - length) - (length > 0.0 ? 0.0f : 3.0f);
  const float x1 = XAt(end) + (length > 0.0 ? 0.0f : 3.0f);
  const edit::Piece& before = edit_.Pieces()[static_cast<std::size_t>(index - 1)];
  const edit::Piece& after = edit_.Pieces()[static_cast<std::size_t>(index)];
  if (length <= 0.0 && std::abs(after.from - before.to) < 1e-6) return {};
  return {x0, track.y - 4.0f, std::max(x1 - x0, 1.0f), track.height + 8.0f};
}

void Editor::KeepPlayheadInView() {
  const double span = Track().width / pxPerSecond_;
  if (playhead_ < viewStart_ || playhead_ > viewStart_ + span) {
    // A page on, as a reader turns one, rather than a slide under the eye.
    viewStart_ = playhead_ - span * 0.1;
  }
  viewStart_ = std::clamp(viewStart_, 0.0, std::max(0.0, edit_.Duration() - span));
}

// ------------------------------------------------------------------ the edit

void Editor::Before() { before_ = edit_; }

void Editor::Changed() {
  undo_.push_back(before_);
  if (undo_.size() > kUndoSteps) undo_.erase(undo_.begin());
  redo_.clear();
  before_ = edit_;
  if (!edit_.Save(folder_)) TraceLog(LOG_WARNING, "EDIT: could not save %s", folder_.c_str());
  playhead_ = std::clamp(playhead_, 0.0, edit_.Duration());
}

void Editor::Cut() {
  Before();
  if (edit_.Cut(playhead_)) Changed();
}

void Editor::ToggleTransition() {
  const int join = selectedJoin_ >= 0 ? selectedJoin_ : selected_;
  Before();
  if (edit_.ToggleTransition(join)) Changed();
}

void Editor::DeleteSelected() {
  if (selectedSound_ >= 0) {
    Before();
    if (edit_.RemoveSound(selectedSound_)) Changed();
    selectedSound_ = -1;
    return;
  }
  if (selectedSpan_ >= 0) {
    Before();
    if (edit_.RemoveSpan(selectedSwitch_, selectedSpan_)) Changed();
    selectedSpan_ = -1;
    return;
  }
  if (selectedKey_ >= 0) {
    Before();
    if (edit_.RemoveKey(selectedTrack_, selectedKey_)) Changed();
    selectedKey_ = -1;
    return;
  }
  if (selectedJoin_ >= 0) {
    // A transition deleted is a cut that snaps.
    if (edit_.Transition(selectedJoin_) > 0.0) ToggleTransition();
    return;
  }
  if (selected_ < 0) return;
  Before();
  const double start = edit_.Start(selected_);
  if (!edit_.RippleDelete(selected_)) return;
  // The playhead goes to where the piece was, which is now whatever followed it.
  playhead_ = std::max(0.0, start - edit_.Transition(selected_));
  selected_ = -1;
  Changed();
}

void Editor::StepUndo(bool redo) {
  std::vector<edit::Edit>& from = redo ? redo_ : undo_;
  std::vector<edit::Edit>& to = redo ? undo_ : redo_;
  if (from.empty()) return;
  to.push_back(edit_);
  edit_ = std::move(from.back());
  from.pop_back();
  before_ = edit_;
  selected_ = selectedJoin_ = selectedKey_ = -1;
  if (!edit_.Save(folder_)) TraceLog(LOG_WARNING, "EDIT: could not save %s", folder_.c_str());
  playhead_ = std::clamp(playhead_, 0.0, edit_.Duration());
}

// ------------------------------------------------------------------ keys

std::vector<float> Editor::ShiftOf(long long box, double at) const {
  const std::vector<float> none = {0.0f, 0.0f, 1.0f};
  const edit::Track*       track = edit_.FindTrack("box:" + std::to_string(box));
  return track != nullptr ? edit::Evaluate(*track, at, none, /*proportional=*/4u) : none;
}

std::vector<App::BoxShift> Editor::ShiftsAt(const std::vector<std::string>& canvas,
                                            double at) const {
  std::vector<App::BoxShift> out;
  bool any = false;
  for (const edit::Track& track : edit_.Tracks()) any = any || track.target.rfind("box:", 0) == 0;
  if (!any) return out;
  const std::vector<recording::BoxPlace> places = recording::Boxes(canvas);
  for (std::size_t i = 0; i < places.size(); ++i) {
    if (places[i].id == 0) continue;
    const std::vector<float> s = ShiftOf(places[i].id, at);
    if (s[0] != 0.0f || s[1] != 0.0f || s[2] != 1.0f) {
      out.push_back({static_cast<int>(i), s[0], s[1], s[2]});
    }
  }
  return out;
}

void Editor::SetBox(long long box, float dx, float dy, float scale) {
  if (!Keyable() || box == 0) return;
  edit_.SetKey("box:" + std::to_string(box), edit_.At(playhead_).source,
               {dx, dy, std::clamp(scale, 0.1f, 10.0f)});
}

bool Editor::Keyable() const { return !edit_.At(playhead_).crossing; }

void Editor::SetCamera(float centreX, float centreY, float height) {
  if (!Keyable()) return;
  edit_.SetKey("camera", edit_.At(playhead_).source, {centreX, centreY, std::max(height, 1e-3f)});
}

void Editor::FreeCamera() {
  if (!Keyable()) return;
  Before();
  edit_.SetFree("camera", edit_.At(playhead_).source);
  Changed();
}

void Editor::SetFace(float x, float y, float height) {
  if (!Keyable()) return;
  edit_.SetKey("face", edit_.At(playhead_).source, {x, y, std::max(height, 0.0f)});
}

void Editor::ToggleFace() {
  if (!Keyable() || faceStream_ == nullptr) return;
  const std::vector<float> face = FaceAt(edit_.At(playhead_).source);
  Before();
  // Taken off where it is, shrinking to nothing; put back where it starts.
  if (face[2] > 0.01f) {
    SetFace(face[0], face[1], 0.0f);
  } else {
    const std::vector<float> back = DefaultFace();
    SetFace(back[0], back[1], back[2]);
  }
  Changed();
}

void Editor::HandleSwitches(Vector2 mouse, bool* consumed) {
  // The settings: a press on a row ticks it, anywhere else puts them away.
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    if (settingsOpen_) {
      for (int i = 0; i < static_cast<int>(std::size(edit::kSwitchKinds)); ++i) {
        if (!CheckCollisionPointRec(mouse, SettingsRow(i))) continue;
        const char*         name = edit::kSwitchKinds[i].name;
        const edit::Switch* s = edit_.FindSwitch(name);
        Before();
        edit_.Tick(name, s == nullptr || !s->ticked);
        Changed();
        *consumed = true;
        return;
      }
      if (CheckCollisionPointRec(mouse,
                                 SettingsRow(static_cast<int>(std::size(edit::kSwitchKinds))))) {
        settingsOpen_ = false;
        *consumed = true;
        const std::string path = filedialog::OpenSound();
        if (!path.empty()) AddSound(path);
        return;
      }
      settingsOpen_ = false;
      *consumed = !CheckCollisionPointRec(mouse, SettingsButton());
      if (*consumed) return;
    }
    if (CheckCollisionPointRec(mouse, SettingsButton())) {
      settingsOpen_ = !settingsOpen_;
      *consumed = true;
      return;
    }
    // A block's edge resizes it, its middle moves it; empty timeline makes one.
    const std::vector<std::string> ticked = Ticked();
    for (int lane = 0; lane < static_cast<int>(ticked.size()); ++lane) {
      if (!CheckCollisionPointRec(mouse, SwitchRow(lane))) continue;
      const edit::Switch* s = edit_.FindSwitch(ticked[static_cast<std::size_t>(lane)]);
      spanSwitch_ = ticked[static_cast<std::size_t>(lane)];
      spanPress_ = TimeAt(mouse.x);
      spanDrag_ = SpanDrag::Make;
      spanIndex_ = -1;
      for (int k = 0; s != nullptr && k < static_cast<int>(s->spans.size()); ++k) {
        const Rectangle r = SpanRect(lane, s->spans[static_cast<std::size_t>(k)]);
        if (mouse.x < r.x - 4.0f || mouse.x > r.x + r.width + 4.0f) continue;
        spanIndex_ = k;
        spanStart_ = s->spans[static_cast<std::size_t>(k)];
        spanDrag_ = std::abs(mouse.x - r.x) < 6.0f                ? SpanDrag::From
                    : std::abs(mouse.x - (r.x + r.width)) < 6.0f ? SpanDrag::To
                                                                  : SpanDrag::Move;
      }
      Before();
      if (spanDrag_ == SpanDrag::Make) {
        const double at = edit_.SourceNear(spanPress_);
        spanIndex_ = edit_.AddSpan(spanSwitch_, {at, at});
        spanStart_ = {at, at};
      }
      selectedSwitch_ = spanSwitch_;
      selectedSpan_ = spanIndex_;
      selected_ = selectedJoin_ = selectedKey_ = -1;
      playing_ = false;
      *consumed = true;
      return;
    }
  }
  if (spanDrag_ == SpanDrag::None) return;
  *consumed = true;
  // Ends move in output time -- where the hand is -- and are kept in the
  // recording's.
  const double     now = std::clamp(TimeAt(mouse.x), 0.0, edit_.Duration());
  const double     moved = now - spanPress_;
  const double     from = edit_.OutputNear(spanStart_.from, true);
  const double     to = edit_.OutputNear(spanStart_.to, false);
  edit::Span       span = spanStart_;
  switch (spanDrag_) {
    case SpanDrag::Make:
      span = {edit_.SourceNear(std::min(spanPress_, now)), edit_.SourceNear(std::max(spanPress_, now))};
      break;
    case SpanDrag::Move:
      span = {edit_.SourceNear(std::clamp(from + moved, 0.0, edit_.Duration())),
              edit_.SourceNear(std::clamp(to + moved, 0.0, edit_.Duration()))};
      break;
    case SpanDrag::From:
      span = {edit_.SourceNear(std::min(now, to)), spanStart_.to};
      break;
    case SpanDrag::To:
      span = {spanStart_.from, edit_.SourceNear(std::max(now, from))};
      break;
    case SpanDrag::None:
      break;
  }
  spanIndex_ = edit_.SetSpan(spanSwitch_, spanIndex_, span);
  selectedSpan_ = spanIndex_;
  playhead_ = now;
  if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
    // A press that made nothing to speak of makes nothing.
    const edit::Switch* s = edit_.FindSwitch(spanSwitch_);
    if (s != nullptr && spanIndex_ >= 0 &&
        s->spans[static_cast<std::size_t>(spanIndex_)].to -
                s->spans[static_cast<std::size_t>(spanIndex_)].from < 0.05) {
      edit_.RemoveSpan(spanSwitch_, spanIndex_);
      selectedSpan_ = -1;
    }
    spanDrag_ = SpanDrag::None;
    Changed();
  }
}

void Editor::AddSound(const std::string& path) {
  const std::string& ffmpeg = video::ToolPath();
  if (ffmpeg.empty()) return;
  // A copy beside the recording, as an mp3 -- which anything plays, the editor
  // included -- so the edit is whole wherever the folder goes.
  const std::filesystem::path folder = std::filesystem::path(folder_) / "sounds";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const std::string stem = std::filesystem::path(path).stem().string();
  std::filesystem::path target = folder / (stem + ".mp3");
  for (int n = 2; std::filesystem::exists(target); ++n) {
    target = folder / (stem + " " + std::to_string(n) + ".mp3");
  }
  proc::Capture(proc::Quote(ffmpeg) + " -y -v error -i " + proc::Quote(path) +
                " -vn -ac 2 -ar 44100 -b:a 192k " + proc::Quote(target.string()));
  const double length = std::atof(proc::Capture(proc::Quote(Ffprobe()) +
                                                " -v error -show_entries format=duration -of csv=p=0 " +
                                                proc::Quote(target.string())).c_str());
  if (!std::filesystem::exists(target) || length <= 0.0) {
    note_ = "Could not read " + std::filesystem::path(path).filename().string();
    noteUntil_ = GetTime() + 6.0;
    return;
  }
  Before();
  edit::Sound sound;
  sound.file = "sounds/" + target.filename().string();
  sound.at = playhead_;
  sound.to = length;
  selectedSound_ = edit_.AddSound(sound);
  selected_ = selectedJoin_ = selectedKey_ = selectedSpan_ = -1;
  Changed();
}

void Editor::HandleSounds(Vector2 mouse, bool* consumed) {
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !*consumed) {
    for (int i = 0; i < static_cast<int>(edit_.Sounds().size()); ++i) {
      if (!CheckCollisionPointRec(mouse, SoundRow(i))) continue;
      *consumed = true;
      selectedSound_ = -1;
      const Rectangle r = SoundRect(i);
      if (mouse.x < r.x - 4.0f || mouse.x > r.x + r.width + 4.0f) return;
      // Its edges trim it; the rest moves it.
      soundDrag_ = std::abs(mouse.x - r.x) < 6.0f                ? SoundDrag::From
                   : std::abs(mouse.x - (r.x + r.width)) < 6.0f ? SoundDrag::To
                                                                 : SoundDrag::Move;
      soundIndex_ = i;
      soundStart_ = edit_.Sounds()[static_cast<std::size_t>(i)];
      soundPress_ = TimeAt(mouse.x);
      selectedSound_ = i;
      selected_ = selectedJoin_ = selectedKey_ = selectedSpan_ = -1;
      playing_ = false;
      Before();
      return;
    }
  }
  if (soundDrag_ == SoundDrag::None) return;
  *consumed = true;
  const double moved = TimeAt(mouse.x) - soundPress_;
  edit::Sound  sound = soundStart_;
  switch (soundDrag_) {
    case SoundDrag::Move:
      sound.at = std::max(0.0, soundStart_.at + moved);
      break;
    case SoundDrag::From: {
      // The start of what plays, and where it plays, move together.
      const double by = std::clamp(moved, -soundStart_.from, soundStart_.Length() - 0.05);
      sound.from = soundStart_.from + by;
      sound.at = std::max(0.0, soundStart_.at + by);
      break;
    }
    case SoundDrag::To:
      sound.to = std::max(soundStart_.from + 0.05, soundStart_.to + moved);
      break;
    case SoundDrag::None:
      break;
  }
  edit_.SetSound(soundIndex_, sound);
  if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
    soundDrag_ = SoundDrag::None;
    Changed();
  }
}

void Editor::PlayLaidSounds() {
  if (!IsAudioDeviceReady()) return;
  const std::vector<edit::Sound>& sounds = edit_.Sounds();
  // One player a sound, kept in step with the edit's list of them.
  while (players_.size() > sounds.size()) {
    if (players_.back().loaded) UnloadMusicStream(players_.back().music);
    players_.pop_back();
  }
  players_.resize(sounds.size());
  for (std::size_t i = 0; i < sounds.size(); ++i) {
    const edit::Sound& sound = sounds[i];
    Player&            player = players_[i];
    if (player.file != sound.file) {
      if (player.loaded) UnloadMusicStream(player.music);
      const std::string path = (std::filesystem::path(folder_) / sound.file).string();
      player = {sound.file, LoadMusicStream(path.c_str()), false, false};
      player.loaded = IsMusicValid(player.music);
      if (player.loaded) player.music.looping = false;
    }
    if (!player.loaded) continue;
    const double into = playhead_ - sound.at;
    const bool   wanted = playing_ && !render_.on && into >= 0.0 && into < sound.Length();
    if (!wanted) {
      if (player.playing) PauseMusicStream(player.music);
      player.playing = false;
      continue;
    }
    SetMusicVolume(player.music, sound.gain);
    const double at = sound.from + into;
    if (!player.playing) {
      PlayMusicStream(player.music);
      SeekMusicStream(player.music, static_cast<float>(at));
      player.playing = true;
    } else if (std::abs(GetMusicTimePlayed(player.music) - at) > 0.25) {
      SeekMusicStream(player.music, static_cast<float>(at));
    }
    UpdateMusicStream(player.music);
  }
}

void Editor::HandleMonitor() {
  const Rectangle monitor = Monitor();
  const Vector2   mouse = GetMousePosition();
  // The monitor shows the frame at its own pixel size: a point on it is a point
  // in the frame, from its corner.
  const Vector2   at = {mouse.x - monitor.x, mouse.y - monitor.y};
  const Rectangle frame = {0.0f, 0.0f, monitor.width, monitor.height};
  const bool      over = CheckCollisionPointRec(mouse, monitor);
  const double    source = edit_.At(playhead_).source;

  if (hold_ == Hold::None && over && Keyable()) {
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && faceStream_ != nullptr) {
      const std::vector<float> face = FaceAt(source);
      const Rectangle          r = FaceRect(face, frame);
      if (r.height > 1.0f && CheckCollisionPointRec(at, r)) {
        // The corner resizes, as every window's grip does; the rest moves.
        const bool corner = at.x > r.x + r.width - 18.0f && at.y > r.y + r.height - 18.0f;
        hold_ = corner ? Hold::FaceSize : Hold::Face;
        holdFrom_ = at;
        holdFace_ = face;
        playing_ = false;
        Before();
      }
    }
    if (hold_ == Hold::None && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      // A box: moved in the picture, as the face is, by a key of its own.
      const int index = app_->BoxUnder(at);
      if (index >= 0 && app_->BoxId(index) != 0) {
        const Rectangle r = app_->BoxRect(index);
        const bool corner = at.x > r.x + r.width - 18.0f && at.y > r.y + r.height - 18.0f;
        hold_ = corner ? Hold::BoxSize : Hold::Box;
        holdBox_ = app_->BoxId(index);
        holdRect_ = r;
        holdFrom_ = at;
        holdFace_ = ShiftOf(holdBox_, source);
        playing_ = false;
        Before();
      }
    } else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
      hold_ = Hold::Camera;
      holdFrom_ = at;
      holdFace_ = {cameraX_, cameraY_, cameraHeight_};
      playing_ = false;
      Before();
    }
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
      // About the pointer: the point of the world under it stays under it. A
      // run of wheel clicks is one step.
      if (GetTime() - lastWheelKey_ > 0.6) Before();
      const float perPixel = cameraHeight_ / monitor.height;
      const float worldX = cameraX_ + (at.x - monitor.width * 0.5f) * perPixel;
      const float worldY = cameraY_ + (at.y - monitor.height * 0.5f) * perPixel;
      const float height = cameraHeight_ / std::pow(1.15f, wheel);
      const float after = height / monitor.height;
      SetCamera(worldX - (at.x - monitor.width * 0.5f) * after,
                worldY - (at.y - monitor.height * 0.5f) * after, height);
      if (GetTime() - lastWheelKey_ > 0.6) Changed(); else edit_.Save(folder_);
      lastWheelKey_ = GetTime();
    }
  }

  const float dx = at.x - holdFrom_.x;
  const float dy = at.y - holdFrom_.y;
  switch (hold_) {
    case Hold::Face:
      SetFace(holdFace_[0] + dx / monitor.height, holdFace_[1] + dy / monitor.height, holdFace_[2]);
      break;
    case Hold::FaceSize: {
      // Its top-left stays put and its shape is kept: the corner follows the
      // hand along the diagonal.
      const Rectangle r = FaceRect(holdFace_, frame);
      const float     along = (dx * r.width + dy * r.height) /
                          std::max(1.0f, r.width * r.width + r.height * r.height);
      const float     scale = std::max(0.15f, 1.0f + along);
      const float     h = holdFace_[2] * scale;
      const float     w = r.width / monitor.height * scale;
      const float     left = r.x / monitor.height;
      const float     top = r.y / monitor.height;
      SetFace(left + w * 0.5f, top + h * 0.5f, h);
      break;
    }
    case Hold::Camera: {
      const float perPixel = holdFace_[2] / monitor.height;
      SetCamera(holdFace_[0] - dx * perPixel, holdFace_[1] - dy * perPixel, holdFace_[2]);
      break;
    }
    case Hold::Box: {
      const float perPixel = cameraHeight_ / monitor.height;
      SetBox(holdBox_, holdFace_[0] + dx * perPixel, holdFace_[1] + dy * perPixel, holdFace_[2]);
      break;
    }
    case Hold::BoxSize: {
      // Its type scaled, and so the box: the corner follows the hand along the
      // diagonal while its top-left stays put.
      const Rectangle r = holdRect_;
      const float along = (dx * r.width + dy * r.height) /
                          std::max(1.0f, r.width * r.width + r.height * r.height);
      SetBox(holdBox_, holdFace_[0], holdFace_[1], holdFace_[2] * std::max(0.15f, 1.0f + along));
      break;
    }
    case Hold::None:
      break;
  }
  const bool released = (hold_ == Hold::Camera) ? IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)
                                                : IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
  if (hold_ != Hold::None && released) {
    hold_ = Hold::None;
    Changed();
  }
}

// ------------------------------------------------------------------ each frame

void Editor::Show() {
  const edit::Moment moment = edit_.At(playhead_);
  const edit::Track* camera = edit_.FindTrack("camera");
  // The camera as the keys have it: centre, and how much of the world's height
  // the frame holds -- which, unlike a zoom, means the same at any size.
  const auto keyed = [camera](recording::View* view, double source) {
    if (view->height <= 0 || view->zoom <= 0.0f) return;
    const std::vector<float> recorded = {view->centreX, view->centreY,
                                         static_cast<float>(view->height) / view->zoom};
    const std::vector<float> held =
        camera != nullptr ? edit::Evaluate(*camera, source, recorded, /*proportional=*/4u) : recorded;
    view->centreX = held[0];
    view->centreY = held[1];
    view->zoom = static_cast<float>(view->height) / held[2];
  };
  const auto viewAt = [this](int frame) {
    recording::View view;
    if (recording_.FrameCount() > 0) view = recording_.FrameRecord(std::max(frame, 0)).view;
    view.built.clear();
    return view;
  };
  if (moment.crossing) {
    // Between two pieces: the canvas where the one before ends, turning into the
    // canvas where the next begins.
    const edit::Piece& before = edit_.Pieces()[static_cast<std::size_t>(moment.piece - 1)];
    const edit::Piece& after = edit_.Pieces()[static_cast<std::size_t>(moment.piece)];
    char key[96];
    std::snprintf(key, sizeof(key), "%d %.4f %.4f", moment.piece, before.to, after.from);
    if (cutKey_ != key) {
      cutKey_ = key;
      const int from = recording_.FrameAt(before.to);
      cutFrame_ = recording_.FrameAt(after.from);
      cutBefore_ = {recording_.CanvasAt(from), viewAt(from)};
      cutAfter_ = {recording_.CanvasAt(cutFrame_), viewAt(cutFrame_)};
    }
    App::CutEnd from = cutBefore_, to = cutAfter_;
    keyed(&from.view, before.to);
    keyed(&to.view, after.from);
    app_->ShowCut(cutKey_, from, to, moment.through, ShiftsAt(from.canvas, before.to),
                  ShiftsAt(to.canvas, after.from));
    const float s = morph::Ease(static_cast<float>(moment.through));
    pointer_ = {from.view.pointerX + (to.view.pointerX - from.view.pointerX) * s,
                from.view.pointerY + (to.view.pointerY - from.view.pointerY) * s};
    pointerKnown_ = std::abs(from.view.pointerX) < 1.0e8f && std::abs(to.view.pointerX) < 1.0e8f;
    // Played on out of it, the piece after carries on from its first frame.
    if (shownFrame_ != cutFrame_) {
      canvas_ = cutAfter_.canvas;
      shownFrame_ = cutFrame_;
    }
    return;
  }
  const int frame = recording_.FrameAt(moment.source);
  // Played straight through, each frame's changes and builds happen in turn;
  // anything else -- a scrub, a cut -- is a jump, and the canvas is put as it
  // was there.
  bool             jumped = false;
  std::vector<int> built;
  if (frame != shownFrame_) {
    if (frame > shownFrame_ && shownFrame_ >= -1 && frame - shownFrame_ <= 4) {
      for (int i = shownFrame_ + 1; i <= frame; ++i) {
        const recording::Frame& f = recording_.FrameRecord(i);
        if (f.changed) recording::Apply(f.change, &canvas_);
        built.insert(built.end(), f.view.built.begin(), f.view.built.end());
      }
    } else {
      canvas_ = recording_.CanvasAt(frame);
      jumped = true;
    }
    shownFrame_ = frame;
  }
  // Before the first frame, it looks as the first frame does.
  recording::View view = viewAt(frame);
  keyed(&view, moment.source);
  if (view.zoom > 0.0f) {
    cameraX_ = view.centreX;
    cameraY_ = view.centreY;
    cameraHeight_ = static_cast<float>(view.height) / view.zoom;
  }
  view.built = std::move(built);
  std::vector<std::string> lastBuilt;
  if (jumped && view.focus >= 0 && view.typed) lastBuilt = recording_.LastBuilt(frame, view.focus);
  app_->ShowRecorded(canvas_, view, jumped, lastBuilt);
  app_->ShiftBoxes(ShiftsAt(canvas_, moment.source));
  pointer_ = {view.pointerX, view.pointerY};
  pointerKnown_ = std::abs(view.pointerX) < 1.0e8f;
}

void Editor::DevKeysStep() {
  while (!devKeys_.empty()) {
    if (devWait_ > 0) {
      --devWait_;
      return;
    }
    std::size_t space = devKeys_.find(' ');
    if (space == std::string::npos) space = devKeys_.size();
    const std::string key = devKeys_.substr(0, space);
    devKeys_.erase(0, std::min(space + 1, devKeys_.size()));
    if (key.rfind("wait:", 0) == 0) {
      devWait_ = std::atoi(key.c_str() + 5);
      return;
    }
    float a = 0.0f, b = 0.0f, c = 0.0f;
    if (key.rfind("at:", 0) == 0) {
      playhead_ = std::atof(key.c_str() + 3);
    } else if (key == "cut") {
      Cut();
    } else if (key.rfind("select:", 0) == 0) {
      selected_ = std::atoi(key.c_str() + 7);
      selectedJoin_ = selectedKey_ = -1;
    } else if (key.rfind("join:", 0) == 0) {
      selectedJoin_ = std::atoi(key.c_str() + 5);
      selected_ = selectedKey_ = -1;
    } else if (key == "toggle") {
      ToggleTransition();
    } else if (key == "delete") {
      DeleteSelected();
    } else if (key == "undo") {
      StepUndo(false);
    } else if (key == "redo") {
      StepUndo(true);
    } else if (key == "play") {
      playing_ = true;
    } else if (key == "render") {
      StartRender();
    } else if (key.rfind("zoom:", 0) == 0) {
      // As the wheel would, about the playhead.
      if (pxPerSecond_ <= 0.0) Fit();
      const float x = XAt(playhead_);
      pxPerSecond_ *= std::atof(key.c_str() + 5);
      viewStart_ = playhead_ - (x - Track().x) / pxPerSecond_;
    } else if (std::sscanf(key.c_str(), "campan:%f,%f", &a, &b) == 2) {
      const float perPixel = cameraHeight_ / static_cast<float>(kVideoHeight);
      Before();
      SetCamera(cameraX_ - a * perPixel, cameraY_ - b * perPixel, cameraHeight_);
      Changed();
    } else if (std::sscanf(key.c_str(), "camzoom:%f", &a) == 1) {
      Before();
      SetCamera(cameraX_, cameraY_, cameraHeight_ / std::max(a, 1e-3f));
      Changed();
    } else if (key == "camfree") {
      FreeCamera();
    } else if (std::sscanf(key.c_str(), "face:%f,%f,%f", &a, &b, &c) == 3) {
      Before();
      SetFace(a, b, c);
      Changed();
    } else if (key == "facetoggle") {
      ToggleFace();
    } else if (int j = 0; std::sscanf(key.c_str(), "trans:%d,%f", &j, &a) == 2) {
      Before();
      edit_.SetTransition(j, a);
      Changed();
    } else if (key.rfind("addsound:", 0) == 0) {
      AddSound(key.substr(9));
    } else if (int n = 0; std::sscanf(key.c_str(), "sound:%d,%f", &n, &a) == 2) {
      if (n >= 0 && n < static_cast<int>(edit_.Sounds().size())) {
        edit::Sound sound = edit_.Sounds()[static_cast<std::size_t>(n)];
        sound.at = a;
        Before();
        edit_.SetSound(n, sound);
        Changed();
      }
    } else if (std::sscanf(key.c_str(), "gain:%d,%f", &n, &a) == 2) {
      if (n >= 0 && n < static_cast<int>(edit_.Sounds().size())) {
        edit::Sound sound = edit_.Sounds()[static_cast<std::size_t>(n)];
        sound.gain = std::pow(10.0f, a / 20.0f);
        Before();
        edit_.SetSound(n, sound);
        Changed();
      }
    } else if (key == "settings") {
      settingsOpen_ = !settingsOpen_;
    } else if (key.rfind("tick:", 0) == 0) {
      const edit::Switch* s = edit_.FindSwitch(key.substr(5));
      Before();
      edit_.Tick(key.substr(5), s == nullptr || !s->ticked);
      Changed();
    } else if (key.rfind("block:", 0) == 0) {
      const std::size_t comma = key.find(',');
      if (comma != std::string::npos &&
          std::sscanf(key.c_str() + comma + 1, "%f,%f", &a, &b) == 2) {
        Before();
        edit_.AddSpan(key.substr(6, comma - 6), {edit_.SourceNear(a), edit_.SourceNear(b)});
        Changed();
      }
    } else if (int box = 0; std::sscanf(key.c_str(), "box:%d,%f,%f,%f", &box, &a, &b, &c) == 4) {
      Before();
      SetBox(app_->BoxId(box), a, b, c);
      Changed();
    }
  }
}

void Editor::Update() {
  if (pxPerSecond_ <= 0.0) Fit();
  DevKeysStep();

  if (render_.on) {
    // A fixed step, whatever the window's frame rate, at the video's size.
    if (IsKeyPressed(KEY_ESCAPE)) {
      StopRender(false);
      return;
    }
    SetScreenOverride(kVideoWidth, kVideoHeight);
    playhead_ = std::min(render_.frame / kVideoRate, edit_.Duration());
    Show();
    return;
  }

  // The canvas is laid out for the monitor's frame -- what it is drawn into next,
  // and what a press on the monitor is measured against.
  const Rectangle monitorRect = Monitor();
  EnsureTarget(&monitor_, static_cast<int>(monitorRect.width), static_cast<int>(monitorRect.height));
  SetScreenOverride(monitor_.width, monitor_.height);

  const bool   ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
  const bool   shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
  const double duration = edit_.Duration();

  if (IsKeyPressed(KEY_SPACE)) {
    if (!playing_ && playhead_ >= duration) playhead_ = 0.0;
    playing_ = !playing_;
  }
  const double step = shift ? 1.0 / 60.0 : 1.0;
  if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) playhead_ -= step;
  if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) playhead_ += step;
  if (IsKeyPressed(KEY_HOME)) playhead_ = 0.0;
  if (IsKeyPressed(KEY_END)) playhead_ = duration;
  if (!ctrl && IsKeyPressed(KEY_C)) Cut();
  if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) DeleteSelected();
  if (IsKeyPressed(KEY_T)) ToggleTransition();
  if (!ctrl && IsKeyPressed(KEY_F)) ToggleFace();
  if (!ctrl && IsKeyPressed(KEY_B)) FreeCamera();
  if (IsKeyPressed(KEY_ESCAPE)) {
    selected_ = selectedJoin_ = selectedKey_ = selectedSpan_ = selectedSound_ = -1;
    settingsOpen_ = false;
  }
  if (ctrl && IsKeyPressed(KEY_Z)) StepUndo(/*redo=*/shift);
  if (ctrl && IsKeyPressed(KEY_Y)) StepUndo(/*redo=*/true);
  if (ctrl && IsKeyPressed(KEY_R)) StartRender();

  const Vector2 mouse = GetMousePosition();
  bool          consumed = false;
  HandleSwitches(mouse, &consumed);
  HandleSounds(mouse, &consumed);
  if (IsFileDropped()) {
    // A sound dropped on the window is laid at the playhead.
    const FilePathList dropped = LoadDroppedFiles();
    for (unsigned int i = 0; i < dropped.count; ++i) AddSound(dropped.paths[i]);
    UnloadDroppedFiles(dropped);
  }
  if (selectedSound_ >= 0 && selectedSound_ < static_cast<int>(edit_.Sounds().size())) {
    // A decibel at a time.
    const int louder = (IsKeyPressed(KEY_UP) ? 1 : 0) - (IsKeyPressed(KEY_DOWN) ? 1 : 0);
    if (louder != 0) {
      edit::Sound sound = edit_.Sounds()[static_cast<std::size_t>(selectedSound_)];
      sound.gain = std::clamp(sound.gain * std::pow(10.0f, static_cast<float>(louder) / 20.0f),
                              0.0f, 8.0f);
      Before();
      edit_.SetSound(selectedSound_, sound);
      Changed();
    }
  }
  const bool    overPanel = CheckCollisionPointRec(mouse, Panel());
  if (overPanel) {
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && shift) {
      viewStart_ -= wheel * 80.0 / pxPerSecond_;
    } else if (wheel != 0.0f) {
      // About the pointer: the moment under it stays under it.
      const double at = TimeAt(mouse.x);
      const double fit = Track().width / std::max(edit_.Duration(), 1e-3);
      pxPerSecond_ = std::clamp(pxPerSecond_ * std::pow(1.25, wheel), fit,
                                std::max(fit, kMaxPxPerSecond));
      viewStart_ = at - (mouse.x - Track().x) / pxPerSecond_;
    }
  } else if (!consumed) {
    HandleMonitor();
  }

  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && overPanel && !consumed) {
    for (int i = 1; i < static_cast<int>(edit_.Pieces().size()); ++i) {
      const Rectangle r = JoinRect(i);
      if (edit_.Transition(i) <= 0.0 || r.width <= 0.0f) continue;
      if (std::abs(mouse.x - (r.x + r.width)) < 5.0f && mouse.y >= r.y && mouse.y <= r.y + r.height) {
        joinDrag_ = i;
        joinStart_ = edit_.Transition(i);
        joinPress_ = TimeAt(mouse.x);
        selectedJoin_ = i;
        selected_ = selectedKey_ = selectedSpan_ = selectedSound_ = -1;
        playing_ = false;
        Before();
        consumed = true;
      }
    }
  }
  if (joinDrag_ >= 0) {
    // The pieces after it move with the hand, so the edge stays under it.
    consumed = true;
    edit_.SetTransition(joinDrag_, std::clamp(joinStart_ + (TimeAt(mouse.x) - joinPress_), 0.1, 5.0));
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      joinDrag_ = -1;
      Changed();
    }
  }
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && overPanel && !consumed) {
    selectedSpan_ = selectedSound_ = -1;
    scrubbing_ = true;
    playing_ = false;
    // A press on a key, a transition or a piece chooses it; one anywhere else
    // puts the choice down. The small things are asked first.
    selected_ = selectedJoin_ = selectedKey_ = -1;
    const Rectangle row = KeyRow();
    for (const edit::Track& track : edit_.Tracks()) {
      const int lane = LaneOf(track.target);
      if (lane < 0 || selectedKey_ >= 0) continue;
      const float y = row.y + kLaneHeight * (static_cast<float>(lane) + 0.5f);
      for (int k = 0; k < static_cast<int>(track.keys.size()); ++k) {
        const double out = edit_.OutputAt(track.keys[static_cast<std::size_t>(k)].at);
        if (out < 0.0) continue;
        if (std::abs(mouse.x - XAt(out)) < 6.0f && std::abs(mouse.y - y) < kLaneHeight * 0.5f) {
          selectedTrack_ = track.target;
          selectedKey_ = k;
          playhead_ = out;
          scrubbing_ = false;
        }
      }
    }
    for (int i = 1; i < static_cast<int>(edit_.Pieces().size()) && selectedKey_ < 0; ++i) {
      if (CheckCollisionPointRec(mouse, JoinRect(i))) selectedJoin_ = i;
    }
    for (int i = 0; i < static_cast<int>(edit_.Pieces().size()) && selectedJoin_ < 0 &&
                    selectedKey_ < 0;
         ++i) {
      if (CheckCollisionPointRec(mouse, PieceRect(i))) selected_ = i;
    }
  }
  if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) scrubbing_ = false;
  if (scrubbing_) playhead_ = TimeAt(mouse.x);

  if (playing_) {
    playhead_ += GetFrameTime();
    if (playhead_ >= edit_.Duration()) playing_ = false;
  }
  playhead_ = std::clamp(playhead_, 0.0, edit_.Duration());
  PlaySound(edit_.At(playhead_));
  PlayLaidSounds();
  // The edit may have got shorter, and a zoom must not show past either end.
  const double fit = Track().width / std::max(edit_.Duration(), 1e-3);
  pxPerSecond_ = std::max(pxPerSecond_, fit);
  if (playing_) KeepPlayheadInView();
  const double span = Track().width / pxPerSecond_;
  viewStart_ = std::clamp(viewStart_, 0.0, std::max(0.0, edit_.Duration() - span));

  Show();
}

void Editor::Draw(FontStore& fonts) {
  if (pxPerSecond_ <= 0.0) Fit();
  constexpr float kEm = 13.0f;
  ClearBackground(kWindowBg);
  if (render_.on) {
    // The frame just sent, fitted to the window, and how far there is to go.
    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight()) - 40.0f;
    const float scale = std::min(w / kVideoWidth, h / kVideoHeight);
    const Rectangle to = {(w - kVideoWidth * scale) * 0.5f, (h - kVideoHeight * scale) * 0.5f,
                          kVideoWidth * scale, kVideoHeight * scale};
    DrawTexturePro(render_.target.resolved.texture,
                   {0.0f, 0.0f, static_cast<float>(kVideoWidth), -static_cast<float>(kVideoHeight)},
                   to, {0.0f, 0.0f}, 0.0f, WHITE);
    char line[128];
    std::snprintf(line, sizeof(line), "Rendering %d of %d frames   Escape stops it",
                  render_.frame, render_.frames);
    text::Draw(fonts, Face::Mono, line, {kInset, static_cast<float>(GetScreenHeight()) - 14.0f},
               kEm, kCodeFg);
    return;
  }

  // The monitor: the frame as it will be in the video.
  const Rectangle monitor = Monitor();
  if (monitor_.resolved.id != 0) {
    DrawTexturePro(monitor_.resolved.texture,
                   {0.0f, 0.0f, static_cast<float>(monitor_.width),
                    -static_cast<float>(monitor_.height)},
                   monitor, {0.0f, 0.0f}, 0.0f, WHITE);
  }
  // The face says it can be taken hold of when pointed at.
  if (faceStream_ != nullptr && Keyable()) {
    const Rectangle frame = {monitor.x, monitor.y, monitor.width, monitor.height};
    const Rectangle r = FaceRect(FaceAt(edit_.At(playhead_).source), frame);
    if (r.height > 1.0f && (hold_ == Hold::Face || hold_ == Hold::FaceSize ||
                            CheckCollisionPointRec(GetMousePosition(), r))) {
      DrawRectangleLinesEx(r, 2.0f, Fade(kPieceChosen, 0.9f));
      DrawGripOnAnything({r.x + r.width - 16.0f, r.y + r.height - 16.0f, 12.0f, 12.0f});
    }
  }

  const Rectangle panel = Panel();
  const Rectangle ruler = Ruler();
  const Rectangle track = Track();
  const Rectangle row = KeyRow();
  DrawRectangleRec(panel, kTimelineBg);

  // A tick every so many seconds, as many as fit at this zoom.
  const double duration = edit_.Duration();
  double       every = 1.0;
  for (double candidate : {0.1, 0.5, 1.0, 5.0, 10.0, 30.0, 60.0, 300.0, 600.0}) {
    every = candidate;
    if (candidate * pxPerSecond_ >= 70.0) break;
  }
  BeginScissorMode(static_cast<int>(ruler.x) - 1, static_cast<int>(panel.y),
                   static_cast<int>(ruler.width) + 2, static_cast<int>(panel.height));
  const double first = std::floor(viewStart_ / every) * every;
  for (double t = first; t <= duration + 1e-9 && XAt(t) <= ruler.x + ruler.width; t += every) {
    const float x = XAt(t);
    DrawLineEx({x, ruler.y + ruler.height - 8.0f}, {x, ruler.y + ruler.height}, 1.0f, kTick);
    text::Draw(fonts, Face::Mono, Clock(t), {x + 3.0f, ruler.y + ruler.height - 6.0f}, kEm, kTick);
  }

  for (int i = 0; i < static_cast<int>(edit_.Pieces().size()); ++i) {
    const Rectangle r = PieceRect(i);
    DrawRectangleRec(r, i == selected_ ? kPieceChosen : kPieceFill);
    // Where in the recording it comes from, when there is room to say.
    const edit::Piece& piece = edit_.Pieces()[static_cast<std::size_t>(i)];
    const std::string  from = Clock(piece.from) + " - " + Clock(piece.to);
    if (text::Measure(fonts, Face::Mono, from, kEm) + 12.0f < r.width) {
      text::Draw(fonts, Face::Mono, from, {r.x + 6.0f, r.y + r.height - 8.0f}, kEm,
                 Fade(kCodeFg, 0.7f));
    }
  }

  // The transitions, over the gaps they fill: a lozenge for one with a length,
  // a sliver for a cut that snaps.
  for (int i = 1; i < static_cast<int>(edit_.Pieces().size()); ++i) {
    const Rectangle r = JoinRect(i);
    if (r.width <= 0.0f) continue;
    const Color fill = i == selectedJoin_ ? kJoinChosen : kJoinFill;
    DrawRectangleRounded(r, 0.35f, 6, edit_.Transition(i) > 0.0 ? fill : Fade(fill, 0.55f));
    if (edit_.Transition(i) > 0.0) {
      char length[16];
      std::snprintf(length, sizeof(length), "%.1fs", edit_.Transition(i));
      if (text::Measure(fonts, Face::Mono, length, 11.0f) + 8.0f < r.width) {
        text::Draw(fonts, Face::Mono, length, {r.x + 4.0f, r.y + r.height - 8.0f}, 11.0f,
                   Fade(kWindowBg, 0.8f));
      }
      // Its right edge is what lengthens it.
      DrawLineEx({r.x + r.width - 2.0f, r.y + 6.0f}, {r.x + r.width - 2.0f, r.y + r.height - 6.0f},
                 2.0f, Fade(kWindowBg, 0.5f));
    }
  }

  // The keys: a lane each for the camera, the face and the boxes, a diamond for
  // a key and a hollow one for handing back to the recording.
  const Color inks[3] = {kCameraKey, kFaceKey, kBoxKey};
  for (int lane = 0; lane < 3; ++lane) {
    const float y = row.y + kLaneHeight * (static_cast<float>(lane) + 0.5f);
    DrawLineEx({row.x, y}, {row.x + row.width, y}, 1.0f, Fade(inks[lane], 0.18f));
  }
  for (const edit::Track& track : edit_.Tracks()) {
    const int lane = LaneOf(track.target);
    if (lane < 0) continue;
    const float y = row.y + kLaneHeight * (static_cast<float>(lane) + 0.5f);
    const Color ink = inks[lane];
    const edit::Track* tr = &track;
    for (int k = 0; k < static_cast<int>(tr->keys.size()); ++k) {
      const edit::Key& key = tr->keys[static_cast<std::size_t>(k)];
      const double     out = edit_.OutputAt(key.at);
      if (out < 0.0) continue;
      const float x = XAt(out);
      const bool  chosen = selectedKey_ == k && selectedTrack_ == track.target;
      const float r = chosen ? 6.0f : 4.5f;
      const Vector2 top = {x, y - r}, right = {x + r, y}, bottom = {x, y + r}, left = {x - r, y};
      if (key.free) {
        DrawLineEx(top, right, 1.5f, ink);
        DrawLineEx(right, bottom, 1.5f, ink);
        DrawLineEx(bottom, left, 1.5f, ink);
        DrawLineEx(left, top, 1.5f, ink);
      } else {
        DrawTriangle(top, left, bottom, chosen ? WHITE : ink);
        DrawTriangle(top, bottom, right, chosen ? WHITE : ink);
      }
    }
  }

  // The switches' timelines, a block wherever one is on.
  const std::vector<std::string> ticked = Ticked();
  for (int lane = 0; lane < static_cast<int>(ticked.size()); ++lane) {
    const Rectangle lr = SwitchRow(lane);
    DrawRectangleRec(lr, Fade(kSpanFill, 0.08f));
    const std::string& name = ticked[static_cast<std::size_t>(lane)];
    if (const edit::Switch* s = edit_.FindSwitch(name)) {
      for (int k = 0; k < static_cast<int>(s->spans.size()); ++k) {
        const Rectangle r = SpanRect(lane, s->spans[static_cast<std::size_t>(k)]);
        const bool      chosen = selectedSwitch_ == name && selectedSpan_ == k;
        DrawRectangleRounded(r, 0.4f, 6, chosen ? kSpanChosen : kSpanFill);
      }
    }
    for (const edit::SwitchKind& kind : edit::kSwitchKinds) {
      if (name == kind.name) {
        text::Draw(fonts, Face::Mono, kind.label, {lr.x + 4.0f, lr.y + lr.height - 4.0f}, 11.0f,
                   Fade(kCodeFg, 0.45f));
      }
    }
  }

  // The sounds, each on its own timeline: what it is, and how loud.
  for (int i = 0; i < static_cast<int>(edit_.Sounds().size()); ++i) {
    const Rectangle    lr = SoundRow(i);
    const edit::Sound& sound = edit_.Sounds()[static_cast<std::size_t>(i)];
    DrawRectangleRec(lr, Fade(kSoundFill, 0.08f));
    const Rectangle r = SoundRect(i);
    DrawRectangleRounded(r, 0.3f, 6, i == selectedSound_ ? kSoundChosen : kSoundFill);
    char label[160];
    std::snprintf(label, sizeof(label), "%s  %+.0f dB",
                  std::filesystem::path(sound.file).stem().string().c_str(),
                  20.0 * std::log10(std::max(sound.gain, 1e-4f)));
    if (text::Measure(fonts, Face::Mono, label, 11.0f) + 10.0f < r.width) {
      text::Draw(fonts, Face::Mono, label, {r.x + 5.0f, r.y + r.height - 5.0f}, 11.0f, kCodeFg);
    }
  }

  const float x = XAt(playhead_);
  const float bottom = !edit_.Sounds().empty()
                           ? SoundRow(static_cast<int>(edit_.Sounds().size()) - 1).y + kSoundLane
                       : ticked.empty() ? row.y + row.height
                                        : SwitchRow(static_cast<int>(ticked.size()) - 1).y + kSwitchLane;
  DrawLineEx({x, ruler.y}, {x, bottom}, 2.0f, kPlayhead);
  EndScissorMode();

  std::string clock = Clock(playhead_) + " / " + Clock(duration) + (playing_ ? "" : "  paused");
  if (!note_.empty() && GetTime() < noteUntil_) clock += "   " + note_;
  text::Draw(fonts, Face::Mono, clock, {panel.x + kInset, panel.y + panel.height - 8.0f}, kEm,
             kCodeFg);
  const char* help =
      "Space play  C cut  Del take out  T transition  F face  B camera back  Ctrl+R render";
  const Rectangle button = SettingsButton();
  const float helpWidth = text::Measure(fonts, Face::Mono, help, kEm);
  text::Draw(fonts, Face::Mono, help, {button.x - 16.0f - helpWidth, panel.y + panel.height - 8.0f},
             kEm, kTick);
  const bool overButton = CheckCollisionPointRec(GetMousePosition(), button);
  DrawRectangleRounded(button, 0.3f, 6, Fade(kPieceFill, settingsOpen_ || overButton ? 1.0f : 0.6f));
  text::Draw(fonts, Face::Mono, "Settings", {button.x + 8.0f, button.y + button.height - 5.0f}, kEm,
             kCodeFg);
  if (settingsOpen_) {
    // The edit's settings: a tick for each switch, which gives it a timeline.
    const int rows = static_cast<int>(std::size(edit::kSwitchKinds)) + 1;
    const Rectangle first = SettingsRow(0);
    const Rectangle back = {first.x - 8.0f, first.y - 8.0f, first.width + 16.0f,
                            first.height * static_cast<float>(rows) + 16.0f};
    DrawRectangleRounded(back, 0.12f, 8, kTimelineBg);
    DrawRectangleRoundedLinesEx(back, 0.12f, 8, 1.0f, Fade(kCodeFg, 0.25f));
    const Rectangle add = SettingsRow(rows - 1);
    text::Draw(fonts, Face::Mono, "Add sound...", {add.x + 28.0f, add.y + add.height - 8.0f}, kEm,
               CheckCollisionPointRec(GetMousePosition(), add) ? kSoundChosen : kCodeFg);
    for (int i = 0; i < rows - 1; ++i) {
      const Rectangle    r = SettingsRow(i);
      const edit::Switch* s = edit_.FindSwitch(edit::kSwitchKinds[i].name);
      const bool         on = s != nullptr && s->ticked;
      const Rectangle    box = {r.x + 4.0f, r.y + 5.0f, 15.0f, 15.0f};
      DrawRectangleRoundedLinesEx(box, 0.25f, 4, 1.5f, kCodeFg);
      if (on) DrawRectangleRounded({box.x + 3.5f, box.y + 3.5f, 8.0f, 8.0f}, 0.25f, 4, kSpanFill);
      text::Draw(fonts, Face::Mono, edit::kSwitchKinds[i].label,
                 {r.x + 28.0f, r.y + r.height - 8.0f}, kEm, kCodeFg);
    }
  }
}

}  // namespace sb
