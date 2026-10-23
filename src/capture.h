// The camera and the microphone, recorded beside a session (recording.h).
//
// ffmpeg does it, as it does everything with video here: a child process reads
// the devices and writes `camera.mkv` into the recording's folder -- the picture
// as H.264, the sound as AAC, in a container that survives being stopped
// abruptly. Nothing is shown while recording (DESIGN.md: the face is not shown
// live).
//
// **Keeping time.** Every packet is stamped with the wall clock as it arrives
// (`use_wallclock_as_timestamps`), so ffmpeg's log says, as its input's `start`,
// the wall-clock time the file's first moment was; the session records the wall
// clock at its own beginning. The difference lines the two up.
//
// Which devices: the ones named `camera` and `microphone` in settings.txt, or
// else the first of each kind DirectShow lists. Raylib-free.
#pragma once

#include <string>

#include "process.h"

namespace sb::capture {

struct Devices {
  std::string camera;       // empty: none
  std::string microphone;
};

// What there is to record with. Asks ffmpeg to list the devices, which opens
// none of them.
Devices Find();

// The wall clock now, in seconds since 1970, as the capture's stamps are.
double WallClock();

class Capture {
 public:
  // Starts recording into `folder`. False, saying why, if there is nothing to
  // record with or ffmpeg would not start -- and then the session is recorded
  // without.
  bool Start(const std::string& folder, std::string* error);
  // Asks ffmpeg to finish the file properly, and waits for it.
  void Stop();
  bool Running() const { return feed_.Running(); }

 private:
  proc::Feed feed_;
};

// Where the camera's file is in a recording's folder, and the wall-clock time
// its first moment was, read from what ffmpeg said. False if there is no
// camera recording, or its log does not say.
std::string CameraFile(const std::string& folder);
bool        CameraStart(const std::string& folder, double* wallClock);

}  // namespace sb::capture
