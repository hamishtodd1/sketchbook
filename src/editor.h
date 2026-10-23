// The editor: a recording played back on the canvas, with a timeline under it.
//
// A mode of the app that feels like a program of its own, started only from the
// command line (`--edit <recording>`), so that what is edited is drawn by the
// very code that drew it. The canvas is passive (App::SetPassive): the pointer
// and the keys are the editor's.
//
// **The monitor** is the video as it will be: the canvas drawn into a 16:9 frame
// of its own (editor_frame.cpp), with the face in its corner, shown above the
// timeline. Rendering draws the same frame at 1920 by 1080.
//
// The timeline is the edit's pieces (edit.h), end to end, with a transition
// wherever the recording jumps, and under them the keys. Space plays and
// pauses; dragging on the timeline scrubs; clicking a piece, a transition or a
// key selects it. C cuts at the playhead; Delete takes the selected piece out
// and closes the gap, makes the selected transition a snap, or removes the
// selected key; T takes a transition away or puts it back, and dragging its
// right edge makes it longer or shorter. On the monitor,
// dragging the face or a box moves it and its corner resizes it, F takes the
// face off or puts it back; the right button drags the camera and the wheel
// zooms it, and B gives the camera back to the recording -- each a key at the
// playhead. Ctrl+Z and
// Ctrl+Y undo and redo. The wheel over the timeline zooms it about the pointer,
// and Shift+wheel slides it. Left and Right step a second (Shift: a frame),
// Home and End go to either end. Ctrl+R renders the edit to `video.mp4` in the
// recording's folder, 1080p at 60 frames a second; Escape stops it. Every
// change is saved at once.
//
// **Settings** (the button by the help) are the edit's: ticking a switch -- the
// mouse pointer -- gives it a timeline of its own under the keys, on which
// dragging makes a block, and the switch is on wherever a block is. A block's
// edges resize it, its middle moves it, Delete takes it out.
//
// **Sounds** -- music, a voiceover -- are added from the settings or by dropping
// a file on the window, at the playhead, each on a timeline of its own. They
// are copied into the recording's folder first, so the edit goes where it goes.
// Dragging one moves it, its edges trim it, Up and Down change how loud it is
// by a decibel, Delete takes it out. They play along and are mixed into the
// render over the microphone.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app.h"
#include "edit.h"
#include "fontstore.h"
#include "glext.h"
#include "process.h"
#include "recording.h"
#include "transition.h"
#include "video.h"

namespace sb {

class Editor {
 public:
  ~Editor() { Close(); }
  bool Open(const std::string& folder, App* app, std::string* error);
  // Lets go of the sound and the GPU's things, which has to happen while the
  // audio device and the window are still open.
  void Close();

  // Before the app's own Update: takes the editor's input, and puts the canvas
  // as it was at the playhead, laid out for the frame it is about to be drawn
  // into.
  void Update();
  // Between the app's Update and the window's drawing: the frame drawn -- the
  // monitor's, or the render's, which is then sent.
  void DrawFrame(App& app);
  // The window: the monitor and the timeline, or how far a render has got.
  void Draw(FontStore& fonts);

  // **Rendering**: every frame of the edit, at a fixed step, drawn at the
  // video's size and handed to ffmpeg. While it runs, the window shows each
  // frame as it goes and how far it has got.
  bool Rendering() const { return render_.on; }

  void SetPlayhead(double seconds) { playhead_ = seconds; }
  void SetPlaying(bool playing) { playing_ = playing; }
  // Dev: things done to the editor, one a frame, space-separated -- `at:S` (the
  // playhead to S seconds), `cut`, `select:N`, `join:N` (the transition into
  // piece N), `toggle`, `delete`, `undo`, `redo`, `play`, `zoom:F` (the
  // timeline, as the wheel, about the playhead), `render`, `campan:DX,DY` (the
  // camera dragged by so many pixels of the video), `camzoom:F`, `camfree`,
  // `face:X,Y,H` (the face put there, in heights of the video), `facetoggle`,
  // `box:N,DX,DY,S` (box N moved by so many world units and its type scaled),
  // `trans:N,S` (the transition into piece N made S seconds long),
  // `addsound:PATH` (no spaces), `sound:N,AT` (sound N moved to output AT),
  // `gain:N,DB`, `settings` (open or shut), `tick:NAME` (a switch ticked, or
  // unticked), `block:NAME,FROM,TO` (a block on
  // its timeline, in output seconds),
  // `wait:N` (frames) -- so an edit can be tested with nobody at it.
  void DevKeys(const std::string& keys) { devKeys_ = keys; }

 private:
  // What a video is rendered as.
  static constexpr int    kVideoWidth = 1920;
  static constexpr int    kVideoHeight = 1080;
  static constexpr double kVideoRate = 60.0;

  // The face's picture and what draws it, made when the editor opens.
  void OpenFace();
  void CloseFace();

  // A frame of the video: drawn multisampled, resolved into a texture.
  struct Target {
    glext::MultisampleTarget msaa;
    RenderTexture2D          resolved{};
    int                      width = 0;
    int                      height = 0;
  };
  void EnsureTarget(Target* target, int width, int height);
  void ReleaseTarget(Target* target);
  // The canvas, then the face, drawn into `target`.
  void Compose(App& app, Target& target, bool waitForFace);

  // The timeline's geometry, and the monitor's.
  Rectangle Panel() const;
  Rectangle Ruler() const;
  Rectangle Track() const;
  Rectangle KeyRow() const;
  // The switches ticked, in the settings' order, and the timeline of each.
  std::vector<std::string> Ticked() const;
  Rectangle SwitchRow(int lane) const;
  Rectangle SpanRect(int lane, const edit::Span& span) const;
  Rectangle SettingsButton() const;
  Rectangle SettingsRow(int index) const;
  void      HandleSwitches(Vector2 mouse, bool* consumed);
  // Sounds laid under the video.
  void      AddSound(const std::string& path);
  Rectangle SoundRow(int index) const;
  Rectangle SoundRect(int index) const;
  void      HandleSounds(Vector2 mouse, bool* consumed);
  void      PlayLaidSounds();
  void      DrawPointer(const Target& target);
  Rectangle Monitor() const;
  double    TimeAt(float x) const;
  float     XAt(double time) const;
  Rectangle PieceRect(int index) const;
  Rectangle JoinRect(int index) const;
  void      Fit();
  void      KeepPlayheadInView();
  void      Show();

  // A change to the edit: kept for undo, and saved. `Before` is taken when a
  // gesture starts, so a drag is one step.
  void      Before();
  void      Changed();
  void      Cut();
  void      DeleteSelected();
  void      ToggleTransition();
  void      StepUndo(bool redo);
  void      DevKeysStep();

  // The camera and the face, as keys at the playhead. Not in a transition,
  // which is not a moment of the recording.
  bool      Keyable() const;
  void      SetCamera(float centreX, float centreY, float height);
  void      SetFace(float x, float y, float height);
  void      ToggleFace();
  void      FreeCamera();
  void      HandleMonitor();
  // The edit's moves of the boxes in `canvas`, at source time `at`.
  std::vector<App::BoxShift> ShiftsAt(const std::vector<std::string>& canvas, double at) const;
  std::vector<float>         ShiftOf(long long box, double at) const;
  void                       SetBox(long long box, float dx, float dy, float scale);
  // The face's rectangle in the frame, in heights of it, at the playhead.
  std::vector<float> FaceAt(double source) const;
  std::vector<float> DefaultFace() const;
  Rectangle FaceRect(const std::vector<float>& face, Rectangle frame) const;

  void      StartRender();
  void      StopRender(bool finished);
  // The sound: the microphone's, played where the playhead is, in pieces as
  // the edit has it and silent across a transition.
  void      PlaySound(const edit::Moment& moment);
  // What the render's sound is made of: the camera's sound cut to the pieces,
  // with silence for each transition, as an ffmpeg filter graph.
  // `firstLaid` is the ffmpeg input the first laid sound is.
  std::string SoundGraph(int firstLaid) const;
  // The face's picture at a moment of the camera's recording; waiting for it
  // when rendering, taking what there is when not.
  void      FaceFrame(double cameraTime, bool wait);

  App*                    app_ = nullptr;
  std::string             folder_;
  recording::Recording    recording_;
  edit::Edit              edit_;
  double                  playhead_ = 0.0;   // output seconds
  bool                    playing_ = false;
  bool                    scrubbing_ = false;
  int                     selected_ = -1;       // a piece
  int                     selectedJoin_ = -1;   // the join into a piece
  // A transition's length being dragged: which, how long it was, from where.
  int                     joinDrag_ = -1;
  double                  joinStart_ = 0.0;
  double                  joinPress_ = 0.0;
  // A key: which track, and which of its keys.
  std::string             selectedTrack_;
  int                     selectedKey_ = -1;
  // What the timeline shows: from `viewStart_` seconds, at so many pixels a
  // second. Fitted to the whole edit until zoomed.
  double                  viewStart_ = 0.0;
  double                  pxPerSecond_ = 0.0;
  std::vector<edit::Edit> undo_, redo_;
  edit::Edit              before_;   // the edit as a gesture found it
  std::string             devKeys_;
  int                     devWait_ = 0;
  // The canvas as of `shownFrame_` (a frame of the recording), carried forward
  // a frame at a time while playing rather than rebuilt from a keyframe.
  int                     shownFrame_ = -2;
  std::vector<std::string> canvas_;
  // The two ends of the transition last shown, kept while it plays.
  std::string             cutKey_;
  App::CutEnd             cutBefore_, cutAfter_;
  int                     cutFrame_ = -1;   // the frame after it
  // The camera as shown at the playhead, keys and all: its centre and how much
  // of the world's height the frame holds.
  float                   cameraX_ = 0.0f, cameraY_ = 0.0f, cameraHeight_ = 1.0f;

  // What a gesture on the monitor is doing.
  enum class Hold { None, Face, FaceSize, Camera, Box, BoxSize };
  Hold                    hold_ = Hold::None;
  long long               holdBox_ = 0;
  Rectangle               holdRect_{};
  Vector2                 holdFrom_{};
  std::vector<float>      holdFace_;
  double                  lastWheelKey_ = -1.0;

  // The settings, and a block being made, moved or resized.
  bool                    settingsOpen_ = false;
  enum class SpanDrag { None, Make, Move, From, To };
  SpanDrag                spanDrag_ = SpanDrag::None;
  std::string             spanSwitch_;
  int                     spanIndex_ = -1;
  edit::Span              spanStart_{};
  double                  spanPress_ = 0.0;   // output seconds
  std::string             selectedSwitch_;
  int                     selectedSpan_ = -1;
  // A sound being moved or trimmed, and the players the sounds play through.
  enum class SoundDrag { None, Move, From, To };
  SoundDrag               soundDrag_ = SoundDrag::None;
  int                     soundIndex_ = -1;
  edit::Sound             soundStart_{};
  double                  soundPress_ = 0.0;
  int                     selectedSound_ = -1;
  struct Player {
    std::string file;
    Music       music{};
    bool        loaded = false;
    bool        playing = false;
  };
  std::vector<Player>     players_;
  // Where the pointer was in the world at the playhead, as the recording has it.
  Vector2                 pointer_{};
  bool                    pointerKnown_ = false;
  transition::OutlineFill pointerFill_;

  Target                  monitor_;
  struct Render {
    bool                       on = false;
    int                        frame = 0;
    int                        frames = 0;
    proc::Feed                 encoder;
    Target                     target;
    std::vector<unsigned char> pixels;
    std::string                part, done;
  };
  Render                  render_;
  // What the last render came to, said for a while under the timeline.
  std::string             note_;
  double                  noteUntil_ = 0.0;

  // The camera's recording, if there is one, and how far its clock is ahead of
  // the session's: a moment `s` into the session is `s + cameraOffset_` into it.
  std::string             camera_;
  bool                    cameraSound_ = false;
  double                  cameraOffset_ = 0.0;
  // Its picture, for the face, and the texture it is drawn from.
  video::Info                    faceInfo_;
  std::unique_ptr<video::Stream> faceStream_;
  Texture2D                      faceTexture_{};
  double                         faceAsked_ = -1.0;
  Shader                         faceShader_{};
  int                            faceRectLoc_ = -1, faceRadiusLoc_ = -1;
  // A small copy of its sound to play while editing, made the first time.
  std::thread             previewMaker_;
  std::atomic<bool>       previewMade_{false};
  Music                   sound_{};
  bool                    soundLoaded_ = false;
  bool                    soundPlaying_ = false;
};

}  // namespace sb
