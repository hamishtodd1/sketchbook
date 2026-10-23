// An edit of a recording: which stretches of it make the video, in order, and
// how one goes into the next.
//
// A recording is never changed (recording.h); an edit says what to make of one,
// and lives beside it as `edit.txt`. An edit is its **pieces** -- each a stretch
// of the recording, from one time to another -- played one after the other.
// Cutting splits the piece under the playhead in two; a ripple delete takes a
// piece out and closes the gap.
//
// Where two pieces meet and the recording jumps -- something was taken out --
// there is a **transition**: time of its own, half a second unless changed, in
// which what was there before the jump moves to what is there after it. Taking
// the transition away (a length of nothing) is how a cut snaps. Two pieces that
// meet without a jump, as a cut leaves them, have none: nothing was skipped.
//
// Times on the video are *output* times; times in the recording are *source*
// times. The two agree until the first transition.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace sb::edit {

inline constexpr double kDefaultTransition = 0.5;   // seconds

// Moves a file just written over another. A file just written can be held for a
// moment by whatever scans new files, and then a move fails; so it is tried
// again, and at worst copied.
bool MoveOver(const std::string& from, const std::string& to);

struct Piece {
  double from = 0.0;   // source seconds
  double to = 0.0;
  // The transition into this piece from the one before, in output seconds, if
  // the recording jumps there. The first piece's is never used.
  double in = kDefaultTransition;

  double Length() const { return to - from; }
  bool   operator==(const Piece& other) const {
    return from == other.from && to == other.to && in == other.in;
  }
};

// **Keyframes.** What something is set to in the video rather than what the
// recording had: the camera, the face. A key is at a moment *of the recording*
// (source seconds), so cutting never moves one -- a key in a piece taken out is
// simply never reached. A key's value is arrived at by its moment, moving on the
// sigmoid over the half second before it (from what the recording had, into a
// first key); between keys a track holds. After its last key it holds. A `free`
// key hands it back to the recording, easing over the half second after. So a
// key is a move made *then*, as a cut's transition is, not a drift from the key
// before.
struct Key {
  double             at = 0.0;
  bool               free = false;
  std::vector<float> value;
};

struct Track {
  std::string      target;   // `camera`, `face`
  std::vector<Key> keys;     // in order of `at`
};

// A track's value at source time `at`, given what the recording has there.
// Components named in `proportional` (a bit each) ease in proportion -- a zoom,
// where halving and doubling are the same size of move.
std::vector<float> Evaluate(const Track& track, double at, const std::vector<float>& recorded,
                            unsigned proportional = 0);

// **Switches**: what is either on or off in the video -- the pointer, to begin
// with. Each is ticked in the edit's settings, which gives it a timeline, and is
// on wherever a block on that timeline is. A block is a stretch of the
// recording (source seconds), as a key is a moment of it, so cutting never moves
// one.
struct Span {
  double from = 0.0;
  double to = 0.0;
};

struct Switch {
  std::string       name;      // `pointer`
  bool              ticked = false;
  std::vector<Span> spans;
};

// The switches there are, and what the settings call them.
struct SwitchKind {
  const char* name;
  const char* label;
};
inline constexpr SwitchKind kSwitchKinds[] = {{"pointer", "Mouse pointer"}};

// **Sounds laid under the video**: music, a voiceover recorded afterwards. Unlike
// everything else in an edit, a sound is placed in the video's own time, not the
// recording's: it is laid against the finished video, so taking a piece out
// does not move it or cut it.
struct Sound {
  std::string file;          // in the recording's folder, `sounds/...`
  double      at = 0.0;      // output seconds where it starts
  double      from = 0.0;    // the stretch of the file that plays
  double      to = 0.0;
  float       gain = 1.0f;   // as a factor

  double Length() const { return to - from; }
};

// Where the video is at an output time: in a piece, at a source time; or in the
// transition into a piece, some fraction of the way through it.
struct Moment {
  int    piece = 0;
  bool   crossing = false;   // in the transition into `piece`
  double source = 0.0;       // when not crossing
  double through = 0.0;      // when crossing: 0 at the start, 1 at the end
};

class Edit {
 public:
  // The whole recording, as one piece.
  void Reset(double duration);

  // Reads `edit.txt` in `folder`, or makes the whole-recording edit if there is
  // none. False only when there is one and it cannot be read.
  bool Load(const std::string& folder, double duration, std::string* error);
  bool Save(const std::string& folder) const;

  const std::vector<Piece>& Pieces() const { return pieces_; }
  // Puts back pieces an undo kept.
  void SetPieces(std::vector<Piece> pieces) { pieces_ = std::move(pieces); }

  // How long the transition into piece `index` is: nothing for the first, or
  // where the recording does not jump.
  double Transition(int index) const;
  double Duration() const;
  // Where piece `index` starts on the video, after its transition.
  double Start(int index) const;
  Moment At(double t) const;

  // Splits the piece playing at `t`. Not within a frame of either end, nor in a
  // transition.
  bool Cut(double t);
  // Takes piece `index` out; everything after it moves up. The last piece is
  // never taken: an edit of nothing is not an edit.
  bool RippleDelete(int index);
  // The transition into piece `index` taken away, or put back; or made so long.
  bool ToggleTransition(int index);
  void SetTransition(int index, double seconds);

  // Where source time `at` is on the video, if it is in a piece; -1 if not.
  double OutputAt(double at) const;
  // The same for one end of a block, which may be in a piece taken out: then
  // the start of the piece after it (`start`) or the end of the one before.
  double OutputNear(double at, bool start) const;
  // The moment of the recording an output time shows; in a transition, the
  // nearer end of it.
  double SourceNear(double t) const;

  const std::vector<Sound>& Sounds() const { return sounds_; }
  int                       AddSound(Sound sound);
  void                      SetSound(int index, Sound sound);
  bool                      RemoveSound(int index);

  const std::vector<Switch>& Switches() const { return switches_; }
  const Switch*              FindSwitch(const std::string& name) const;
  void                       Tick(const std::string& name, bool ticked);
  // Whether a switch is on at source time `at`: ticked, and in a block.
  bool                       On(const std::string& name, double at) const;
  // A block put in, changed or taken out; `SetSpan` and `AddSpan` say where it
  // now is among them, kept in order.
  int                        AddSpan(const std::string& name, Span span);
  int                        SetSpan(const std::string& name, int index, Span span);
  bool                       RemoveSpan(const std::string& name, int index);

  const std::vector<Track>& Tracks() const { return tracks_; }
  const Track*              FindTrack(const std::string& target) const;
  // A key at `at`: the one already within a frame of it is changed, or one is
  // put in.
  void SetKey(const std::string& target, double at, std::vector<float> value);
  void SetFree(const std::string& target, double at);
  bool RemoveKey(const std::string& target, int index);

 private:
  Key& KeyAt(const std::string& target, double at);

  Switch& SwitchNamed(const std::string& name);

  std::vector<Piece>  pieces_;
  std::vector<Track>  tracks_;
  std::vector<Switch> switches_;
  std::vector<Sound>  sounds_;
};

}  // namespace sb::edit
