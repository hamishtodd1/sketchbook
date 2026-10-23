// Playing a video file, by asking ffmpeg for its pixels.
//
// There is no decoder in here and there is not going to be one. ffmpeg is
// already the answer to "what formats does this open", and the app already
// knows how to run a child process and read what it says -- that is how a code
// box is compiled. A video is the same arrangement with a great deal more
// output: `ffmpeg` is asked for raw RGB frames and they are read off a pipe as
// they arrive, so nothing has to be decoded to disk first and nothing is held
// in memory but the handful of frames that have not been shown yet.
//
// Like the rest of the toolchain layer this file knows nothing about raylib.
// It hands back bytes; whoever wants a texture out of them owns the GPU.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sb::video {

// Where ffmpeg is. Empty if it could not be found, in which case a video window
// says so rather than showing nothing.
const std::string& ToolPath();

struct Info {
  bool   ok = false;
  int    width = 0;
  int    height = 0;
  double duration = 0.0;   // seconds
  double fps = 25.0;
};

// Asks ffprobe what the file is. Cheap: it reads headers, not frames.
Info Probe(const std::string& path);

// One video being played. The frames are decoded on a worker thread into a
// bounded queue -- bounded because a queue that grows with how long the app has
// been open is a leak with extra steps -- and taken from it by whoever is
// drawing, at whatever moment they have reached.
class Stream {
 public:
  Stream(std::string path, Info info, int maxWidth);
  ~Stream();
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  // Jump. The decoder is restarted at `seconds`, which is why scrubbing is a
  // little coarse and playing straight through is not.
  void Seek(double seconds);

  // Bring the picture up to `seconds`. Returns true if the pixels changed, so
  // the caller only re-uploads a texture when there is something new in it.
  bool Advance(double seconds);

  bool                              HaveFrame() const { return haveFrame_; }
  // When the frame on show is from, in the file's seconds; below 0 before one.
  double                            CurrentTime() const { return currentTime_; }
  const std::vector<unsigned char>& Pixels() const { return current_; }  // RGB8
  int                               Width() const { return outWidth_; }
  int                               Height() const { return outHeight_; }
  // True once the decoder has run off the end of the file at this seek point.
  bool                              Ended() const;

 private:
  struct Frame {
    double                     time = 0.0;
    std::vector<unsigned char> rgb;
  };

  void Worker();
  std::string Command(double startAt) const;

  std::string path_;
  Info        info_;
  int         outWidth_ = 0;
  int         outHeight_ = 0;
  std::size_t frameBytes_ = 0;

  mutable std::mutex      mutex_;
  std::condition_variable cv_;
  std::deque<Frame>       queue_;
  // A seek bumps the generation. Frames tagged with an older one are from a
  // decode nobody is waiting for any more and are dropped on sight.
  int              generation_ = 0;
  int              queuedGeneration_ = -1;
  double           startAt_ = 0.0;
  bool             ended_ = false;
  std::atomic<bool> quit_{false};

  // The main thread's copy: the frame actually on screen.
  std::vector<unsigned char> current_;
  bool                       haveFrame_ = false;
  double                     currentTime_ = -1.0;

  std::thread thread_;
};

}  // namespace sb::video
