#include "edit.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sb::edit {
namespace {

constexpr const char* kHeader = "sbedit 1";
constexpr const char* kEditFile = "edit.txt";
constexpr double      kFrame = 1.0 / 60.0;

}  // namespace

// ------------------------------------------------------------------ keys

std::vector<float> Evaluate(const Track& track, double at, const std::vector<float>& recorded,
                            unsigned proportional) {
  const std::vector<Key>& keys = track.keys;
  if (keys.empty()) return recorded;
  const auto mix = [&](const std::vector<float>& a, const std::vector<float>& b, double s) {
    const float f = static_cast<float>(std::clamp(s, 0.0, 1.0));
    const float e = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    std::vector<float> out(std::min(a.size(), b.size()));
    for (std::size_t i = 0; i < out.size(); ++i) {
      if (((proportional >> i) & 1u) != 0 && a[i] > 0.0f && b[i] > 0.0f) {
        out[i] = std::exp(std::log(a[i]) + (std::log(b[i]) - std::log(a[i])) * e);
      } else {
        out[i] = a[i] + (b[i] - a[i]) * e;
      }
    }
    return out;
  };
  const double ease = kDefaultTransition;
  // The last key at or before `at`, and the next after it.
  int last = -1;
  for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
    if (keys[static_cast<std::size_t>(i)].at <= at) last = i;
  }
  const Key* before = last >= 0 ? &keys[static_cast<std::size_t>(last)] : nullptr;
  const Key* after = last + 1 < static_cast<int>(keys.size())
                         ? &keys[static_cast<std::size_t>(last + 1)]
                         : nullptr;
  // What it is when no key is holding it: the recording's, or on its way back
  // there from the key held before a `free`.
  std::vector<float> base = recorded;
  if (before != nullptr && !before->free) {
    // Held, and on its way to the next key only in the half second before it --
    // or all the way, if they are nearer than that.
    if (after != nullptr && !after->free) {
      const double start = std::max(before->at, after->at - ease);
      if (at <= start) return before->value;
      return mix(before->value, after->value, (at - start) / (after->at - start));
    }
    return before->value;
  }
  if (before != nullptr && before->free && last > 0) {
    const Key& held = keys[static_cast<std::size_t>(last - 1)];
    if (!held.free && at < before->at + ease) base = mix(held.value, recorded, (at - before->at) / ease);
  }
  if (after != nullptr && !after->free && at >= after->at - ease) {
    return mix(base, after->value, (at - (after->at - ease)) / ease);
  }
  return base;
}

const Track* Edit::FindTrack(const std::string& target) const {
  for (const Track& track : tracks_) {
    if (track.target == target) return &track;
  }
  return nullptr;
}

Key& Edit::KeyAt(const std::string& target, double at) {
  Track* track = nullptr;
  for (Track& t : tracks_) {
    if (t.target == target) track = &t;
  }
  if (track == nullptr) {
    tracks_.push_back({target, {}});
    track = &tracks_.back();
  }
  for (Key& key : track->keys) {
    if (std::abs(key.at - at) < kFrame) return key;
  }
  const auto place = std::find_if(track->keys.begin(), track->keys.end(),
                                  [at](const Key& key) { return key.at > at; });
  return *track->keys.insert(place, Key{at, false, {}});
}

void Edit::SetKey(const std::string& target, double at, std::vector<float> value) {
  Key& key = KeyAt(target, at);
  key.free = false;
  key.value = std::move(value);
}

void Edit::SetFree(const std::string& target, double at) {
  Key& key = KeyAt(target, at);
  key.free = true;
  key.value.clear();
}

bool Edit::RemoveKey(const std::string& target, int index) {
  for (Track& track : tracks_) {
    if (track.target != target || index < 0 || index >= static_cast<int>(track.keys.size())) continue;
    track.keys.erase(track.keys.begin() + index);
    return true;
  }
  return false;
}

double Edit::OutputAt(double at) const {
  for (int i = 0; i < static_cast<int>(pieces_.size()); ++i) {
    const Piece& piece = pieces_[static_cast<std::size_t>(i)];
    if (at >= piece.from && at <= piece.to) return Start(i) + (at - piece.from);
  }
  return -1.0;
}

double Edit::OutputNear(double at, bool start) const {
  const double exact = OutputAt(at);
  if (exact >= 0.0) return exact;
  for (int i = 0; i < static_cast<int>(pieces_.size()); ++i) {
    const Piece& piece = pieces_[static_cast<std::size_t>(i)];
    if (at < piece.from) {
      if (start || i == 0) return Start(i);
      return Start(i - 1) + pieces_[static_cast<std::size_t>(i - 1)].Length();
    }
  }
  return Duration();
}

double Edit::SourceNear(double t) const {
  const Moment moment = At(t);
  if (!moment.crossing) return moment.source;
  const Piece& before = pieces_[static_cast<std::size_t>(moment.piece - 1)];
  const Piece& after = pieces_[static_cast<std::size_t>(moment.piece)];
  return moment.through < 0.5 ? before.to : after.from;
}

// ------------------------------------------------------------------ sounds

int Edit::AddSound(Sound sound) {
  sounds_.push_back(std::move(sound));
  return static_cast<int>(sounds_.size()) - 1;
}

void Edit::SetSound(int index, Sound sound) {
  if (index < 0 || index >= static_cast<int>(sounds_.size())) return;
  sound.at = std::max(0.0, sound.at);
  sound.to = std::max(sound.to, sound.from + kFrame);
  sounds_[static_cast<std::size_t>(index)] = std::move(sound);
}

bool Edit::RemoveSound(int index) {
  if (index < 0 || index >= static_cast<int>(sounds_.size())) return false;
  sounds_.erase(sounds_.begin() + index);
  return true;
}

// ------------------------------------------------------------------ switches

const Switch* Edit::FindSwitch(const std::string& name) const {
  for (const Switch& s : switches_) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

Switch& Edit::SwitchNamed(const std::string& name) {
  for (Switch& s : switches_) {
    if (s.name == name) return s;
  }
  switches_.push_back({name, false, {}});
  return switches_.back();
}

void Edit::Tick(const std::string& name, bool ticked) { SwitchNamed(name).ticked = ticked; }

bool Edit::On(const std::string& name, double at) const {
  const Switch* s = FindSwitch(name);
  if (s == nullptr || !s->ticked) return false;
  for (const Span& span : s->spans) {
    if (at >= span.from && at <= span.to) return true;
  }
  return false;
}

int Edit::AddSpan(const std::string& name, Span span) {
  Switch& s = SwitchNamed(name);
  if (span.to < span.from) std::swap(span.from, span.to);
  const auto place = std::find_if(s.spans.begin(), s.spans.end(),
                                  [&span](const Span& other) { return other.from > span.from; });
  return static_cast<int>(s.spans.insert(place, span) - s.spans.begin());
}

int Edit::SetSpan(const std::string& name, int index, Span span) {
  Switch& s = SwitchNamed(name);
  if (index < 0 || index >= static_cast<int>(s.spans.size())) return -1;
  s.spans.erase(s.spans.begin() + index);
  return AddSpan(name, span);
}

bool Edit::RemoveSpan(const std::string& name, int index) {
  Switch& s = SwitchNamed(name);
  if (index < 0 || index >= static_cast<int>(s.spans.size())) return false;
  s.spans.erase(s.spans.begin() + index);
  return true;
}

// ------------------------------------------------------------------ pieces

void Edit::Reset(double duration) {
  pieces_ = {{0.0, duration}};
  tracks_.clear();
  switches_.clear();
  sounds_.clear();
}

bool Edit::Load(const std::string& folder, double duration, std::string* error) {
  Reset(duration);
  const std::filesystem::path path = std::filesystem::path(folder) / kEditFile;
  if (!std::filesystem::exists(path)) return true;
  std::ifstream in(path, std::ios::binary);
  std::string   line;
  if (!in || !std::getline(in, line) || line.rfind(kHeader, 0) != 0) {
    if (error != nullptr) *error = path.string() + " is not an edit";
    return false;
  }
  std::vector<Piece> pieces;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("key ", 0) == 0) {
      // `key <target> <at> free`, or `key <target> <at> <value...>`.
      std::istringstream fields(line.substr(4));
      std::string        target, word;
      double             at = 0.0;
      if (!(fields >> target >> at)) continue;
      std::vector<float> value;
      bool               free = false;
      while (fields >> word) {
        if (word == "free") free = true;
        else value.push_back(std::stof(word));
      }
      if (free) SetFree(target, at);
      else if (!value.empty()) SetKey(target, at, std::move(value));
      continue;
    }
    if (line.rfind("sound ", 0) == 0) {
      // `sound <at> <from> <to> <gain> <file>`, the file last: it has spaces.
      std::istringstream fields(line.substr(6));
      Sound              sound;
      if (fields >> sound.at >> sound.from >> sound.to >> sound.gain) {
        std::getline(fields >> std::ws, sound.file);
        if (!sound.file.empty()) AddSound(std::move(sound));
      }
      continue;
    }
    if (line.rfind("switch ", 0) == 0) {
      // `switch <name> on` -- ticked in the settings -- or `off`.
      std::istringstream fields(line.substr(7));
      std::string        name, state;
      if (fields >> name >> state) Tick(name, state == "on");
      continue;
    }
    if (line.rfind("block ", 0) == 0) {
      std::istringstream fields(line.substr(6));
      std::string        name;
      Span               span;
      if (fields >> name >> span.from >> span.to) AddSpan(name, span);
      continue;
    }
    if (line.rfind("piece ", 0) != 0) continue;   // what a later version adds
    std::istringstream fields(line.substr(6));
    Piece              piece;
    if (!(fields >> piece.from >> piece.to)) continue;
    // Without one written, the transition is the default.
    if (!(fields >> piece.in)) piece.in = kDefaultTransition;
    // A piece is kept to the recording it is of.
    piece.from = std::clamp(piece.from, 0.0, duration);
    piece.to = std::clamp(piece.to, piece.from, duration);
    piece.in = std::max(piece.in, 0.0);
    if (piece.Length() > 0.0) pieces.push_back(piece);
  }
  if (!pieces.empty()) pieces_ = std::move(pieces);
  return true;
}

bool Edit::Save(const std::string& folder) const {
  // Written beside the file and moved over it, as a canvas is, so a crash in
  // the middle leaves the old edit whole.
  const std::filesystem::path path = std::filesystem::path(folder) / kEditFile;
  const std::filesystem::path fresh = path.string() + ".new";
  {
    std::FILE* file = std::fopen(fresh.string().c_str(), "wb");
    if (file == nullptr) return false;
    std::fprintf(file, "%s\n", kHeader);
    for (const Piece& piece : pieces_) {
      std::fprintf(file, "piece %.4f %.4f %.4f\n", piece.from, piece.to, piece.in);
    }
    for (const Sound& sound : sounds_) {
      std::fprintf(file, "sound %.4f %.4f %.4f %.4f %s\n", sound.at, sound.from, sound.to,
                   sound.gain, sound.file.c_str());
    }
    for (const Switch& s : switches_) {
      std::fprintf(file, "switch %s %s\n", s.name.c_str(), s.ticked ? "on" : "off");
      for (const Span& span : s.spans) {
        std::fprintf(file, "block %s %.4f %.4f\n", s.name.c_str(), span.from, span.to);
      }
    }
    for (const Track& track : tracks_) {
      for (const Key& key : track.keys) {
        std::fprintf(file, "key %s %.4f", track.target.c_str(), key.at);
        if (key.free) std::fprintf(file, " free");
        for (float v : key.value) std::fprintf(file, " %.6g", v);
        std::fprintf(file, "\n");
      }
    }
    if (std::fclose(file) != 0) return false;
  }
  return MoveOver(fresh.string(), path.string());
}

bool MoveOver(const std::string& from, const std::string& to) {
  std::error_code ec;
  for (int attempt = 0; attempt < 5; ++attempt) {
    ec.clear();
    std::filesystem::rename(from, to, ec);
    if (!ec) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ec.clear();
  std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) return false;
  std::filesystem::remove(from, ec);
  return true;
}

double Edit::Transition(int index) const {
  if (index <= 0 || index >= static_cast<int>(pieces_.size())) return 0.0;
  const Piece& before = pieces_[static_cast<std::size_t>(index - 1)];
  const Piece& piece = pieces_[static_cast<std::size_t>(index)];
  // Within a frame counts as no jump: a cut puts the two ends at one time.
  if (std::abs(piece.from - before.to) < 1e-6) return 0.0;
  return piece.in;
}

double Edit::Duration() const {
  double total = 0.0;
  for (int i = 0; i < static_cast<int>(pieces_.size()); ++i) {
    total += Transition(i) + pieces_[static_cast<std::size_t>(i)].Length();
  }
  return total;
}

double Edit::Start(int index) const {
  double start = 0.0;
  for (int i = 0; i < index && i < static_cast<int>(pieces_.size()); ++i) {
    start += Transition(i) + pieces_[static_cast<std::size_t>(i)].Length();
  }
  return start + Transition(index);
}

Moment Edit::At(double t) const {
  Moment moment;
  if (pieces_.empty()) {
    moment.source = t;
    return moment;
  }
  double at = 0.0;
  for (int i = 0; i < static_cast<int>(pieces_.size()); ++i) {
    const Piece& piece = pieces_[static_cast<std::size_t>(i)];
    const double crossing = Transition(i);
    moment.piece = i;
    if (t < at + crossing) {
      moment.crossing = true;
      moment.through = (t - at) / crossing;
      return moment;
    }
    at += crossing;
    if (t < at + piece.Length() || i + 1 == static_cast<int>(pieces_.size())) {
      moment.source = std::clamp(piece.from + (t - at), piece.from, piece.to);
      return moment;
    }
    at += piece.Length();
  }
  return moment;
}

bool Edit::Cut(double t) {
  const Moment moment = At(t);
  if (moment.crossing) return false;
  const Piece piece = pieces_[static_cast<std::size_t>(moment.piece)];
  const double at = moment.source;
  if (at - piece.from < kFrame || piece.to - at < kFrame) return false;
  pieces_[static_cast<std::size_t>(moment.piece)].to = at;
  pieces_.insert(pieces_.begin() + moment.piece + 1, Piece{at, piece.to, kDefaultTransition});
  return true;
}

bool Edit::RippleDelete(int index) {
  if (pieces_.size() <= 1 || index < 0 || index >= static_cast<int>(pieces_.size())) return false;
  pieces_.erase(pieces_.begin() + index);
  return true;
}

bool Edit::ToggleTransition(int index) {
  if (index <= 0 || index >= static_cast<int>(pieces_.size())) return false;
  Piece& piece = pieces_[static_cast<std::size_t>(index)];
  piece.in = piece.in > 0.0 ? 0.0 : kDefaultTransition;
  return true;
}

}  // namespace sb::edit
