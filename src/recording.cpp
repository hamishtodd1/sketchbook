#include "recording.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sb::recording {
namespace {

constexpr const char* kHeader = "sbrecording 1";
constexpr const char* kSessionFile = "session.txt";
// A keyframe of the canvas every this many frames, in memory, for seeking.
constexpr int kKeyEvery = 600;

void WriteLines(std::FILE* file, const std::vector<std::string>& lines) {
  for (const std::string& line : lines) {
    std::fputs("| ", file);
    std::fputs(line.c_str(), file);
    std::fputc('\n', file);
  }
}

}  // namespace

bool View::operator==(const View& o) const {
  return centreX == o.centreX && centreY == o.centreY && zoom == o.zoom && width == o.width &&
         height == o.height && pointerX == o.pointerX && pointerY == o.pointerY &&
         held == o.held && pressed == o.pressed && focus == o.focus && caret == o.caret && built == o.built &&
         typed == o.typed;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    out.push_back(text.substr(at, end - at));
    at = end + 1;
  }
  return out;
}

std::vector<std::string> BoxText(const std::vector<std::string>& canvas, int index,
                                 std::vector<std::string>* rest) {
  std::vector<std::string> text;
  int  box = -1;
  bool inside = false;
  for (const std::string& line : canvas) {
    if (line.rfind("block ", 0) == 0) {
      ++box;
      inside = box == index;
    } else if (inside && line.rfind("> ", 0) == 0) {
      text.push_back(line.substr(2));
      continue;
    } else if (line.rfind("> ", 0) != 0) {
      inside = inside && line != "end";
    }
    if (rest != nullptr) rest->push_back(line);
  }
  return text;
}

std::vector<BoxPlace> Boxes(const std::vector<std::string>& canvas) {
  std::vector<BoxPlace> out;
  for (const std::string& line : canvas) {
    if (line.rfind("block ", 0) != 0) continue;
    std::istringstream fields(line.substr(6));
    BoxPlace           place;
    float              skip = 0.0f;
    fields >> place.x >> place.y >> place.em;
    for (int i = 0; i < 6; ++i) fields >> skip;   // reading, bold, italic, justify, width, source
    fields >> place.id;
    out.push_back(place);
  }
  return out;
}

std::vector<std::pair<std::string, float>> BoxValues(const std::vector<std::string>& canvas,
                                                     int index) {
  std::vector<std::pair<std::string, float>> out;
  int  box = -1;
  bool inside = false;
  for (const std::string& line : canvas) {
    if (line.rfind("block ", 0) == 0) {
      ++box;
      inside = box == index;
    } else if (line == "end") {
      inside = false;
    } else if (inside && line.rfind("val ", 0) == 0) {
      std::istringstream fields(line.substr(4));
      std::string        name;
      float              value = 0.0f;
      if (fields >> name >> value) out.emplace_back(name, value);
    }
  }
  return out;
}

Change Difference(const std::vector<std::string>& before, const std::vector<std::string>& after) {
  std::size_t start = 0;
  while (start < before.size() && start < after.size() && before[start] == after[start]) ++start;
  std::size_t endB = before.size(), endA = after.size();
  while (endB > start && endA > start && before[endB - 1] == after[endA - 1]) {
    --endB;
    --endA;
  }
  Change change;
  change.from = static_cast<int>(start);
  change.removed = static_cast<int>(endB - start);
  change.added.assign(after.begin() + static_cast<std::ptrdiff_t>(start),
                      after.begin() + static_cast<std::ptrdiff_t>(endA));
  return change;
}

void Apply(const Change& change, std::vector<std::string>* lines) {
  const std::size_t from = std::min<std::size_t>(static_cast<std::size_t>(change.from), lines->size());
  const std::size_t to = std::min(lines->size(), from + static_cast<std::size_t>(change.removed));
  lines->erase(lines->begin() + static_cast<std::ptrdiff_t>(from),
               lines->begin() + static_cast<std::ptrdiff_t>(to));
  lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(from), change.added.begin(),
                change.added.end());
}

// ------------------------------------------------------------------ writing

bool Recorder::Begin(const std::string& folder, const std::string& canvasPath,
                     const std::string& canvas, double wallClock, std::string* error) {
  End();
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const std::filesystem::path path = std::filesystem::path(folder) / kSessionFile;
  if (std::filesystem::exists(path)) {
    if (error != nullptr) *error = path.string() + " already exists; a recording is never written over";
    return false;
  }
  file_ = std::fopen(path.string().c_str(), "wb");
  if (file_ == nullptr) {
    if (error != nullptr) *error = "could not write " + path.string();
    return false;
  }
  lines_ = SplitLines(canvas);
  std::fprintf(file_, "%s\nwallclock %.6f\ncanvas %s\nlines %zu\n", kHeader, wallClock,
               canvasPath.c_str(), lines_.size());
  WriteLines(file_, lines_);
  wroteView_ = false;
  std::fflush(file_);
  return true;
}

void Recorder::Frame(double time, const std::string& canvas, const View& view) {
  if (file_ == nullptr) return;
  std::vector<std::string> lines = SplitLines(canvas);
  const bool changed = lines != lines_;
  if (!changed && wroteView_ && view == view_) return;

  // A frame: when, then the view. The caret goes last, since it has spaces in it.
  std::fprintf(file_, "f %.4f %.9g %.9g %.9g %d %d %.9g %.9g %d %d %s\n", time, view.centreX,
               view.centreY, view.zoom, view.width, view.height, view.pointerX, view.pointerY,
               (view.held ? 1 : 0) | (view.typed ? 2 : 0) | (view.pressed ? 4 : 0), view.focus,
               view.caret.c_str());
  if (!view.built.empty()) {
    std::fputs("b", file_);
    for (int box : view.built) std::fprintf(file_, " %d", box);
    std::fputc('\n', file_);
  }
  if (changed) {
    const Change change = Difference(lines_, lines);
    std::fprintf(file_, "d %d %d %zu\n", change.from, change.removed, change.added.size());
    WriteLines(file_, change.added);
    lines_ = std::move(lines);
  }
  view_ = view;
  wroteView_ = true;
  if (++sinceFlush_ >= 30) {
    sinceFlush_ = 0;
    std::fflush(file_);
  }
}

void Recorder::End(double time) {
  if (file_ == nullptr) return;
  if (time >= 0.0) std::fprintf(file_, "end %.4f\n", time);
  std::fclose(file_);
  file_ = nullptr;
}

// ------------------------------------------------------------------ reading

bool Recording::Load(const std::string& folder, std::string* error) {
  const std::filesystem::path path = std::filesystem::path(folder) / kSessionFile;
  std::ifstream in(path, std::ios::binary);
  const auto fail = [&](const std::string& why) {
    if (error != nullptr) *error = path.string() + ": " + why;
    return false;
  };
  if (!in) return fail("could not be read");
  std::string line;
  if (!std::getline(in, line) || line != kHeader) return fail("is not a recording");

  // `count` lines of canvas, each written after "| ".
  const auto readLines = [&in](std::size_t count, std::vector<std::string>* out) {
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
      if (!std::getline(in, text)) return false;
      if (!text.empty() && text.back() == '\r') text.pop_back();
      out->push_back(text.size() >= 2 ? text.substr(2) : std::string());
    }
    return true;
  };

  frames_.clear();
  ended_ = 0.0;
  wallClock_ = 0.0;
  initial_.clear();
  keys_.clear();
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("wallclock ", 0) == 0) {
      wallClock_ = std::stod(line.substr(10));
    } else if (line.rfind("end ", 0) == 0) {
      ended_ = std::stod(line.substr(4));
    } else if (line.rfind("canvas ", 0) == 0) {
      canvasPath_ = line.substr(7);
    } else if (line.rfind("lines ", 0) == 0) {
      if (!readLines(std::stoul(line.substr(6)), &initial_)) return fail("ends inside its canvas");
    } else if (line.rfind("f ", 0) == 0) {
      Frame frame;
      std::istringstream fields(line.substr(2));
      int held = 0;
      fields >> frame.time >> frame.view.centreX >> frame.view.centreY >> frame.view.zoom >>
          frame.view.width >> frame.view.height >> frame.view.pointerX >> frame.view.pointerY >>
          held >> frame.view.focus;
      // Flags: 1 a gesture held, 2 the focused box typed in since its build.
      frame.view.held = (held & 1) != 0;
      frame.view.typed = (held & 2) != 0;
      frame.view.pressed = (held & 4) != 0;
      std::getline(fields >> std::ws, frame.view.caret);
      frames_.push_back(std::move(frame));
    } else if (line.rfind("b ", 0) == 0 && !frames_.empty()) {
      std::istringstream fields(line.substr(2));
      for (int box = 0; fields >> box;) frames_.back().view.built.push_back(box);
    } else if (line.rfind("d ", 0) == 0 && !frames_.empty()) {
      Change& change = frames_.back().change;
      std::size_t added = 0;
      std::istringstream fields(line.substr(2));
      fields >> change.from >> change.removed >> added;
      // A crash can cut the last change short; what was written is still kept.
      readLines(added, &change.added);
      frames_.back().changed = true;
    }
  }

  // The keyframes: the canvas as it stood every so many frames.
  std::vector<std::string> canvas = initial_;
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    if (frames_[i].changed) Apply(frames_[i].change, &canvas);
    if ((i + 1) % kKeyEvery == 0) keys_.push_back({static_cast<int>(i), canvas});
  }
  return true;
}

std::vector<std::string> Recording::LastBuilt(int index, int box) const {
  for (int i = std::min(index, FrameCount() - 1); i >= 0; --i) {
    const std::vector<int>& built = frames_[static_cast<std::size_t>(i)].view.built;
    if (std::find(built.begin(), built.end(), box) != built.end()) {
      return BoxText(CanvasAt(i), box);
    }
  }
  return {};
}

int Recording::FrameAt(double time) const {
  const auto after = std::upper_bound(frames_.begin(), frames_.end(), time,
                                      [](double t, const Frame& f) { return t < f.time; });
  return static_cast<int>(after - frames_.begin()) - 1;
}

std::vector<std::string> Recording::CanvasAt(int index) const {
  std::vector<std::string> canvas = initial_;
  int from = 0;
  for (const Key& key : keys_) {
    if (key.frame > index) break;
    canvas = key.lines;
    from = key.frame + 1;
  }
  for (int i = from; i <= index && i < static_cast<int>(frames_.size()); ++i) {
    if (frames_[static_cast<std::size_t>(i)].changed) Apply(frames_[static_cast<std::size_t>(i)].change, &canvas);
  }
  return canvas;
}

}  // namespace sb::recording
