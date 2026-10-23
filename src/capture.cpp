#include "capture.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "settings.h"
#include "video.h"

namespace sb::capture {
namespace {

constexpr const char* kCameraFile = "camera.mkv";
constexpr const char* kCameraLog = "camera.log";

// `[dshow @ ...] "Name" (video)` -- the first of each kind.
Devices ParseList(const std::string& listing) {
  Devices found;
  std::istringstream lines(listing);
  std::string        line;
  while (std::getline(lines, line)) {
    const std::size_t open = line.find('"');
    const std::size_t close = open == std::string::npos ? open : line.find('"', open + 1);
    if (close == std::string::npos || line.find("Alternative name") != std::string::npos) continue;
    const std::string name = line.substr(open + 1, close - open - 1);
    if (line.find("(video)", close) != std::string::npos && found.camera.empty()) {
      found.camera = name;
    } else if (line.find("(audio)", close) != std::string::npos && found.microphone.empty()) {
      found.microphone = name;
    }
  }
  return found;
}

}  // namespace

double WallClock() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

Devices Find() {
  Devices devices;
  const std::string& ffmpeg = video::ToolPath();
  if (!ffmpeg.empty()) {
    // The listing comes on stderr, and ends in an error about the dummy input.
    devices = ParseList(proc::Capture("cmd /c \"" + proc::Quote(ffmpeg) +
                                      " -hide_banner -list_devices true -f dshow -i dummy 2>&1\""));
  }
  const std::string camera = settings::Value("camera");
  const std::string microphone = settings::Value("microphone");
  if (!camera.empty()) devices.camera = camera == "none" ? std::string() : camera;
  if (!microphone.empty()) devices.microphone = microphone == "none" ? std::string() : microphone;
  return devices;
}

bool Capture::Start(const std::string& folder, std::string* error) {
  const std::string& ffmpeg = video::ToolPath();
  if (ffmpeg.empty()) {
    if (error != nullptr) *error = "no ffmpeg on the path";
    return false;
  }
  const std::filesystem::path out = std::filesystem::path(folder) / kCameraFile;
  std::string input;
  // Dev: a picture and a tone ffmpeg makes up, so recording can be tested
  // without anybody's camera or microphone being turned on. Only the picture is
  // stamped by the wall clock: `-re` paces an input by its stamps, and a tone
  // stamped "now" is never early, so it would be made faster than it plays.
  const char* test = std::getenv("SKETCHBOOK_TEST_CAPTURE");
  // `fail` asks for a source that is not there, for seeing what a failed
  // capture looks like.
  if (test != nullptr && std::string(test) == "fail") {
    input = " -f lavfi -i nosuchsource";
  } else if (test != nullptr && *test != '\0') {
    input = " -re -use_wallclock_as_timestamps 1 -f lavfi -i testsrc2=size=640x360:rate=30"
            " -re -f lavfi -i sine=frequency=440:sample_rate=48000";
  } else {
    const Devices devices = Find();
    if (devices.camera.empty() && devices.microphone.empty()) {
      if (error != nullptr) *error = "no camera or microphone";
      return false;
    }
    std::string spec;
    if (!devices.camera.empty()) spec += "video=" + devices.camera;
    if (!devices.microphone.empty()) spec += (spec.empty() ? "" : ":") + std::string("audio=") + devices.microphone;
    input = " -f dshow -use_wallclock_as_timestamps 1 -rtbufsize 512M -i " + proc::Quote(spec);
  }
  // Cheap to encode while the app is busy, at a quality a corner of the picture
  // can afford.
  const std::string command = proc::Quote(ffmpeg) + " -hide_banner -y" + input +
                              " -c:v libx264 -preset ultrafast -crf 23 -pix_fmt yuv420p"
                              " -c:a aac -b:a 192k " +
                              proc::Quote(out.string());
  if (!feed_.Start(command, (std::filesystem::path(folder) / kCameraLog).string())) {
    if (error != nullptr) *error = "ffmpeg would not start";
    return false;
  }
  return true;
}

void Capture::Stop() {
  if (!feed_.Running()) return;
  // `q` is ffmpeg's own "stop and finish the file".
  const unsigned char quit[] = {'q', '\n'};
  feed_.Write(quit, sizeof(quit));
  feed_.Finish();
}

std::string CameraFile(const std::string& folder) {
  const std::filesystem::path path = std::filesystem::path(folder) / kCameraFile;
  return std::filesystem::exists(path) ? path.string() : std::string();
}

bool CameraStart(const std::string& folder, double* wallClock) {
  // `Duration: N/A, start: 1761234567.123456, bitrate: ...` -- the earliest of
  // the inputs, which is where the file's own time begins.
  std::ifstream log(std::filesystem::path(folder) / kCameraLog, std::ios::binary);
  std::string   line;
  bool          found = false;
  while (std::getline(log, line)) {
    const std::size_t at = line.find("start: ");
    if (at == std::string::npos || line.find("Duration") == std::string::npos) continue;
    const double start = std::atof(line.c_str() + at + 7);
    if (start <= 0.0) continue;
    if (!found || start < *wallClock) *wallClock = start;
    found = true;
  }
  return found;
}

}  // namespace sb::capture
