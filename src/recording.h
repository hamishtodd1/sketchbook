// A recorded session: the canvas as it changed, frame by frame, with where the
// camera, the pointer and the caret were.
//
// Not the keys that changed it. Replaying keys would need every compile, reply
// and clock to come out the same a second time; the canvas as text -- what an
// undo step already is -- needs nothing to. A frame is written only when
// something changed, and the canvas only as the lines that did.
//
// The file is text, like a canvas: a header, the canvas it started from, then a
// record per frame that changed anything. It is written as it goes and flushed
// often, so a crash keeps everything up to the last few frames.
#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sb::recording {

// What is seen besides the canvas, at one frame.
struct View {
  float       centreX = 0.0f, centreY = 0.0f, zoom = 1.0f;   // the camera
  int         width = 0, height = 0;                        // the window, pixels
  float       pointerX = -1.0e9f, pointerY = -1.0e9f;       // world
  // Whether a gesture was in flight, which is what turns hovering off; and
  // whether the mouse button was down, which an edit can show.
  bool        held = false;
  bool        pressed = false;
  int         focus = -1;   // which box has the caret, by its place in the canvas
  std::string caret;        // that box's carets (CodeBlock::CaretState)
  // Whether that box has been typed in since it was last built. Sharing leaves
  // such a box alone (App::ShareAcrossCanvas), so a playback has to know.
  bool        typed = false;
  // The boxes asked to build on this frame. A playback builds them then, and
  // otherwise leaves boxes as they are, so it shows no build the session did
  // not have -- a box typed from nothing is not built letter by letter.
  std::vector<int> built;

  bool operator==(const View& other) const;
};

// Lines [from, from + removed) of the canvas became `added`.
struct Change {
  int                      from = 0;
  int                      removed = 0;
  std::vector<std::string> added;
};

struct Frame {
  double              time = 0.0;   // seconds since the recording began
  View                view;
  bool                changed = false;
  Change              change;
};

// The lines `before` would have to change by to become `after`: everything
// between their common start and their common end.
Change Difference(const std::vector<std::string>& before, const std::vector<std::string>& after);
void   Apply(const Change& change, std::vector<std::string>* lines);
std::vector<std::string> SplitLines(const std::string& text);
// The `index`-th box's text in a canvas as a recording keeps it, and (if
// `rest`) the canvas without it -- to tell typing in one box from any other
// change, and to find what a box was built from.
std::vector<std::string> BoxText(const std::vector<std::string>& canvas, int index,
                                 std::vector<std::string>* rest = nullptr);
// Every box's place in a canvas as a recording keeps it, in order.
struct BoxPlace {
  long long id = 0;
  float     x = 0.0f, y = 0.0f, em = 0.0f;
};
std::vector<BoxPlace> Boxes(const std::vector<std::string>& canvas);
// The knob values the `index`-th box's record keeps (its `val` lines).
std::vector<std::pair<std::string, float>> BoxValues(const std::vector<std::string>& canvas,
                                                     int index);

class Recorder {
 public:
  Recorder() = default;
  ~Recorder() { End(); }
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  // Starts `folder` (made if need be) as a recording of the canvas at
  // `canvasPath`, whose text is `canvas` now.
  // `wallClock` is when it begins, in seconds since 1970, which is what lines it
  // up with the camera's recording (capture.h).
  bool Begin(const std::string& folder, const std::string& canvasPath, const std::string& canvas,
             double wallClock, std::string* error);
  // One frame, at `time` seconds since Begin. Written only if something changed.
  void Frame(double time, const std::string& canvas, const View& view);
  // Ends it at `time`, which is how long it lasted: the last frames may have
  // changed nothing, and so not have been written.
  void End(double time = -1.0);
  bool Recording() const { return file_ != nullptr; }

 private:
  std::FILE*               file_ = nullptr;
  std::vector<std::string> lines_;
  View                     view_;
  bool                     wroteView_ = false;
  int                      sinceFlush_ = 0;
};

// A recording read back, for playing and editing.
class Recording {
 public:
  bool Load(const std::string& folder, std::string* error);

  const std::string& CanvasPath() const { return canvasPath_; }
  // When it began, in seconds since 1970; 0 if it did not say.
  double             WallClock() const { return wallClock_; }
  double             Duration() const {
    return std::max(ended_, frames_.empty() ? 0.0 : frames_.back().time);
  }
  int                FrameCount() const { return static_cast<int>(frames_.size()); }
  // The last frame at or before `time`; -1 before the first.
  int                FrameAt(double time) const;
  const Frame&       FrameRecord(int index) const { return frames_[static_cast<std::size_t>(index)]; }
  // The canvas as it was once frame `index` had happened (-1: as it began).
  std::vector<std::string> CanvasAt(int index) const;
  // What box `box` was last built from, as of frame `index`; empty if it had
  // not been built since the recording began.
  std::vector<std::string> LastBuilt(int index, int box) const;

 private:
  std::string              canvasPath_;
  double                   ended_ = 0.0;
  double                   wallClock_ = 0.0;
  std::vector<std::string> initial_;
  std::vector<Frame>       frames_;
  // The canvas every so many frames, so seeking applies a few changes rather
  // than all of them since the beginning.
  struct Key {
    int                      frame;
    std::vector<std::string> lines;
  };
  std::vector<Key>         keys_;
};

}  // namespace sb::recording
