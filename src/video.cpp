#include "video.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "process.h"

namespace sb::video {
namespace {

// How many decoded frames may sit ahead of the picture. Enough to ride out a
// slow frame, few enough that a paused video is not holding a film in RAM.
constexpr std::size_t kQueueDepth = 8;

// Frames are decoded at preview size, not at source size: nothing on this
// canvas is shown at 4K, and the pipe would be the bottleneck if they were.
constexpr int kMaxFrameRate = 30;

std::string FindTool(const char* name) {
  // An explicit path wins, exactly as the C++ toolchain is found.
  if (const char* env = std::getenv("SKETCHBOOK_FFMPEG"); env != nullptr && *env != '\0') {
    std::string base(env);
    const std::size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) return base.substr(0, slash + 1) + name + ".exe";
  }
  // Otherwise trust PATH: a bare name is resolved by CreateProcess.
  return std::string(name);
}

// Parses ffprobe's `key=value` output, which is what
// `-of default=noprint_wrappers=1` produces.
std::string Field(const std::string& text, const std::string& key) {
  std::istringstream stream(text);
  std::string        line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1);
  }
  return {};
}

// avg_frame_rate comes back as a rational, e.g. "30000/1001".
double Rational(const std::string& value) {
  const std::size_t slash = value.find('/');
  if (slash == std::string::npos) return std::atof(value.c_str());
  const double num = std::atof(value.substr(0, slash).c_str());
  const double den = std::atof(value.substr(slash + 1).c_str());
  if (den <= 0.0) return 0.0;
  return num / den;
}

std::string Seconds(double value) {
  std::ostringstream out;
  out.precision(3);
  out << std::fixed << std::max(0.0, value);
  return out.str();
}

}  // namespace

const std::string& ToolPath() {
  static const std::string path = FindTool("ffmpeg");
  return path;
}

Info Probe(const std::string& path) {
  Info info;

  const std::string probe = FindTool("ffprobe");
  const std::string command = probe +
                              " -v error -select_streams v:0"
                              " -show_entries stream=width,height,avg_frame_rate"
                              " -show_entries format=duration"
                              " -of default=noprint_wrappers=1 " +
                              proc::Quote(path);
  const proc::Result result = proc::Run(command, 8000);
  if (!result.started) return info;

  info.width = std::atoi(Field(result.output, "width").c_str());
  info.height = std::atoi(Field(result.output, "height").c_str());
  info.duration = std::atof(Field(result.output, "duration").c_str());
  const double fps = Rational(Field(result.output, "avg_frame_rate"));
  if (fps > 0.1) info.fps = std::min<double>(fps, kMaxFrameRate);
  info.ok = info.width > 0 && info.height > 0;
  return info;
}

// ---------------------------------------------------------------------------

Stream::Stream(std::string path, Info info, int maxWidth)
    : path_(std::move(path)), info_(info) {
  const int width = std::max(info_.width, 1);
  const int height = std::max(info_.height, 1);
  const double scale = std::min(1.0, static_cast<double>(maxWidth) / static_cast<double>(width));
  // ffmpeg's scaler wants even dimensions for most pixel formats, and rawvideo
  // is happier with them too.
  outWidth_ = std::max(2, (static_cast<int>(width * scale) / 2) * 2);
  outHeight_ = std::max(2, (static_cast<int>(height * scale) / 2) * 2);
  frameBytes_ = static_cast<std::size_t>(outWidth_) * static_cast<std::size_t>(outHeight_) * 3;

  thread_ = std::thread([this] { Worker(); });
}

Stream::~Stream() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
    ++generation_;   // whatever is decoding is no longer wanted
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

std::string Stream::Command(double startAt) const {
  std::ostringstream cmd;
  // -ss before -i is the fast seek: ffmpeg jumps in the container rather than
  // decoding its way there, which is what makes scrubbing bearable.
  cmd << ToolPath() << " -hide_banner -loglevel error -ss " << Seconds(startAt) << " -i "
      << proc::Quote(path_) << " -an -sn -vf scale=" << outWidth_ << ":" << outHeight_ << " -r "
      << info_.fps << " -f rawvideo -pix_fmt rgb24 -";
  return cmd.str();
}

void Stream::Worker() {
  int served = -1;
  std::vector<unsigned char> buffer(frameBytes_);

  for (;;) {
    int    generation = 0;
    double start = 0.0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return quit_ || generation_ != served; });
      if (quit_) return;
      generation = generation_;
      start = startAt_;
    }
    served = generation;

    const auto stale = [&] {
      if (quit_) return true;
      std::lock_guard<std::mutex> lock(mutex_);
      return generation_ != generation;
    };

    proc::Pipe pipe;
    if (!pipe.Start(Command(start))) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation_ == generation) ended_ = true;
      continue;
    }

    double time = start;
    while (!stale()) {
      // Wait for room before reading, so a paused video stops pulling on the
      // pipe rather than filling memory with frames nobody asked for.
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] {
          return quit_ || generation_ != generation || queue_.size() < kQueueDepth;
        });
        if (quit_ || generation_ != generation) break;
      }

      if (pipe.Read(buffer.data(), frameBytes_, stale) != frameBytes_) {
        // Short read: the file is over, or the decode was abandoned.
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ == generation) ended_ = true;
        break;
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ != generation) break;
        queue_.push_back({time, buffer});
        queuedGeneration_ = generation;
      }
      time += 1.0 / std::max(1.0, info_.fps);
    }
    // The child goes when this scope does, whether the file ended or the seek
    // moved on: a decoder nobody is reading is a process leak.
  }
}

void Stream::Seek(double seconds) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    startAt_ = std::max(0.0, seconds);
    queue_.clear();
    queuedGeneration_ = -1;
    ended_ = false;
    currentTime_ = -1.0;
  }
  cv_.notify_all();
}

bool Stream::Advance(double seconds) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queuedGeneration_ != generation_) queue_.clear();

    // Take every frame whose moment has passed, and keep the last of them: if
    // the app dropped behind, catching up beats playing in slow motion.
    while (!queue_.empty() && queue_.front().time <= seconds) {
      current_ = std::move(queue_.front().rgb);
      currentTime_ = queue_.front().time;
      queue_.pop_front();
      haveFrame_ = true;
      changed = true;
    }
    // Nothing has been shown yet: show the first frame there is rather than a
    // blank window while the clock catches up with it.
    if (!haveFrame_ && !queue_.empty()) {
      current_ = std::move(queue_.front().rgb);
      currentTime_ = queue_.front().time;
      queue_.pop_front();
      haveFrame_ = true;
      changed = true;
    }
  }
  if (changed) cv_.notify_all();   // there is room for more now
  return changed;
}

bool Stream::Ended() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ended_ && queue_.empty();
}

}  // namespace sb::video
