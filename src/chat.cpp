#include "chat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "blocks.h"
#include "grid.h"
#include "hover.h"
#include "prompt.h"
#include "selection.h"
#include "settings.h"
#include "text.h"
#include "theme.h"

namespace sb {
namespace {

// Everything in a conversation is measured in world units from this size of
// type, so a window is the same shape at every zoom and only drawing scales.
constexpr float kEm = 20.0f;
constexpr float kCodeEm = kEm * 0.9f;
constexpr float kPad = 0.8f * kEm;
constexpr float kHeader = 2.3f * kEm;
constexpr float kTurnGap = 1.0f * kEm;
constexpr float kInputPad = 0.45f * kEm;
constexpr float kSendSize = 1.5f * kEm;
constexpr int   kMaxInputRows = 8;
constexpr float kDefaultWidth = 30.0f * kEm;
constexpr float kDefaultHeight = 34.0f * kEm;
constexpr float kMinWidth = 16.0f * kEm;
constexpr float kMinHeight = 14.0f * kEm;
// A cache that grows with what is said needs a bound.
constexpr std::size_t kCacheMax = 2000;

constexpr Color kFg = kCodeFg;
constexpr Color kMuted = {150, 148, 128, 255};
constexpr Color kBubble = kChromeBg;
constexpr Color kNoteFg = {240, 130, 140, 255};
constexpr Color kCodeTint = kTokType;
constexpr Color kAccentFg = {122, 162, 247, 255};

float LineH() { return kEm * 1.3f; }

// ------------------------------------------------------------ inline text

// One unbreakable piece of a paragraph: a word, a formula, or a span of code.
struct Atom {
  enum class Kind { Text, Math, Code };
  Kind        kind = Kind::Text;
  std::string text;
  bool        bold = false;
  bool        spaceBefore = false;
};

// Splits a paragraph into words and formulas. `$...$` and `\(...\)` are
// mathematics, `**` toggles bold, `` `...` `` is code, and `\$` is a dollar.
std::vector<Atom> Inline(const std::string& s) {
  struct Segment {
    Atom::Kind  kind;
    std::string text;
    bool        bold;
  };
  std::vector<Segment> segments;
  std::string          text;
  bool                 bold = false;
  const auto flush = [&]() {
    if (!text.empty()) segments.push_back({Atom::Kind::Text, text, bold});
    text.clear();
  };
  for (std::size_t i = 0; i < s.size();) {
    if (s.compare(i, 2, "**") == 0) {
      flush();
      bold = !bold;
      i += 2;
    } else if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '$') {
      text += '$';
      i += 2;
    } else if (s[i] == '$') {
      std::size_t j = i + 1;
      while (j < s.size() && !(s[j] == '$' && s[j - 1] != '\\')) ++j;
      if (j < s.size() && j > i + 1) {
        flush();
        segments.push_back({Atom::Kind::Math, s.substr(i + 1, j - i - 1), false});
        i = j + 1;
      } else {
        text += '$';
        ++i;
      }
    } else if (s.compare(i, 2, "\\(") == 0) {
      const std::size_t j = s.find("\\)", i + 2);
      if (j != std::string::npos) {
        flush();
        segments.push_back({Atom::Kind::Math, s.substr(i + 2, j - i - 2), false});
        i = j + 2;
      } else {
        text += s[i++];
      }
    } else if (s[i] == '`') {
      const std::size_t j = s.find('`', i + 1);
      if (j != std::string::npos) {
        flush();
        segments.push_back({Atom::Kind::Code, s.substr(i + 1, j - i - 1), false});
        i = j + 1;
      } else {
        text += s[i++];
      }
    } else {
      text += s[i++];
    }
  }
  flush();

  std::vector<Atom> atoms;
  bool              pending = false;
  for (const Segment& segment : segments) {
    if (segment.kind != Atom::Kind::Text) {
      atoms.push_back({segment.kind, segment.text, false, pending});
      pending = false;
      continue;
    }
    std::string word;
    bool        wordSpace = pending;
    for (const char c : segment.text) {
      if (c == ' ' || c == '\t') {
        if (!word.empty()) atoms.push_back({Atom::Kind::Text, word, segment.bold, wordSpace});
        word.clear();
        pending = true;
        wordSpace = true;
      } else {
        if (word.empty()) wordSpace = pending;
        word += c;
        pending = false;
      }
    }
    if (!word.empty()) atoms.push_back({Atom::Kind::Text, word, segment.bold, wordSpace});
  }
  return atoms;
}

std::string Trim(const std::string& s) {
  const std::size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return {};
  const std::size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

// ------------------------------------------------------------ the input

// The draft broken into rows, as byte ranges that partition it: a hard line
// break ends a row, and a long line wraps at its spaces.
std::vector<std::pair<std::size_t, std::size_t>> Rows(FontStore& fonts, const std::string& s,
                                                      float maxW) {
  std::vector<std::pair<std::size_t, std::size_t>> rows;
  std::size_t                                      begin = 0;
  while (true) {
    std::size_t end = s.find('\n', begin);
    const bool  last = (end == std::string::npos);
    if (last) end = s.size();
    // Wrap [begin, end) greedily, breaking after a space.
    std::size_t from = begin;
    while (true) {
      std::size_t fit = from;
      std::size_t lastSpace = std::string::npos;
      std::size_t i = from;
      while (i < end) {
        std::size_t next = i;
        text::NextCodepoint(s, &next);
        if (text::Measure(fonts, Face::MainRegular,
                          std::string_view(s).substr(from, next - from), kEm) > maxW &&
            i > from) {
          break;
        }
        if (s[i] == ' ') lastSpace = next;
        i = next;
        fit = i;
      }
      if (fit >= end) {
        rows.emplace_back(from, end);
        break;
      }
      const std::size_t cut = (lastSpace != std::string::npos && lastSpace > from) ? lastSpace : fit;
      rows.emplace_back(from, cut);
      from = cut;
    }
    if (last) break;
    begin = end + 1;
  }
  return rows;
}

std::size_t PrevCodepoint(const std::string& s, std::size_t at) {
  if (at == 0) return 0;
  --at;
  while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) --at;
  return at;
}

std::size_t NextCodepointAt(const std::string& s, std::size_t at) {
  if (at >= s.size()) return s.size();
  text::NextCodepoint(s, &at);
  return at;
}

void PutUtf8(int cp, std::string* out) {
  const unsigned int c = static_cast<unsigned int>(cp);
  if (c < 0x80) {
    out->push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (c >> 6)));
    out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (c >> 12)));
    out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (c >> 18)));
    out->push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

bool Triggered(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }

Rectangle ScreenRectOf(const ChatWindow& window, const Viewport& viewport) {
  const Vector2 at = viewport.ToScreen(window.worldPos);
  return {at.x, at.y, window.width * viewport.Zoom(), window.height * viewport.Zoom()};
}

std::string Tokens(long n) {
  char buffer[32];
  if (n >= 1000) std::snprintf(buffer, sizeof(buffer), "%.1fk", static_cast<double>(n) / 1000.0);
  else std::snprintf(buffer, sizeof(buffer), "%ld", n);
  return buffer;
}

std::string Dollars(double d) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), d < 0.1 ? "$%.3f" : "$%.2f", d);
  return buffer;
}

int EffortIndex(const std::string& effort) {
  for (int i = 0; i < claude::kEffortCount; ++i) {
    if (effort == claude::kEfforts[i]) return i;
  }
  return 1;
}

}  // namespace

// ================================================================= the panel

ChatPanel::~ChatPanel() { Clear(); }

Vector2 ChatPanel::DefaultSize() { return {kDefaultWidth, kDefaultHeight}; }

void ChatPanel::New(Vector2 worldCentre, float zoom) {
  auto window = std::make_unique<ChatWindow>();
  window->width = kDefaultWidth;
  window->height = kDefaultHeight;
  window->worldPos =
      SnapToGrid({worldCentre.x - kDefaultWidth * 0.5f, worldCentre.y - kDefaultHeight * 0.5f},
                 zoom);
  window->conversation = std::make_unique<claude::Conversation>();
  window->id = nextId_++;
  windows_.push_back(std::move(window));
  editing_ = static_cast<int>(windows_.size()) - 1;
  changed_ = true;
}

void ChatPanel::Say(const std::string& text, const std::string& effort) {
  if (windows_.empty()) return;
  ChatWindow& window = *windows_.back();
  if (!effort.empty()) window.conversation->effort = effort;
  window.draft = text;
  window.caret = text.size();
  Send(window);
}

void ChatPanel::Rewind(std::size_t index, std::size_t turn, bool canvas) {
  if (index >= windows_.size()) return;
  ChatWindow& window = *windows_[index];
  std::string words;
  std::string before;
  if (!window.conversation->Rewind(turn, &words, &before)) return;
  // What was said goes back into the input, to be said again or changed.
  window.draft = words;
  window.caret = words.size();
  window.laid.clear();
  window.following = true;
  editing_ = static_cast<int>(index);
  if (canvas && !before.empty()) {
    canvasRewind_ = std::move(before);
    canvasRewindDue_ = true;
  }
  rewind_ = {};
  changed_ = true;
}

bool ChatPanel::TakeCanvasRewind(std::string* canvas) {
  if (!canvasRewindDue_) return false;
  canvasRewindDue_ = false;
  *canvas = std::move(canvasRewind_);
  canvasRewind_.clear();
  return true;
}

std::vector<json::Value> ChatPanel::Save() const {
  std::vector<json::Value> out;
  for (const std::unique_ptr<ChatWindow>& window : windows_) {
    json::Value saved = json::Value::Object();
    saved.Set("x", window->worldPos.x);
    saved.Set("y", window->worldPos.y);
    saved.Set("width", window->width);
    saved.Set("height", window->height);
    saved.Set("madeBelow", window->madeBelow);
    saved.Set("conversation", window->conversation->Save());
    out.push_back(std::move(saved));
  }
  return out;
}

void ChatPanel::Adopt(const std::vector<json::Value>& saved) {
  for (const json::Value& one : saved) {
    auto window = std::make_unique<ChatWindow>();
    window->worldPos = {static_cast<float>(one["x"].Number()), static_cast<float>(one["y"].Number())};
    window->width = std::max(kMinWidth, static_cast<float>(one["width"].Number(kDefaultWidth)));
    window->height = std::max(kMinHeight, static_cast<float>(one["height"].Number(kDefaultHeight)));
    window->madeBelow = static_cast<float>(one["madeBelow"].Number());
    window->conversation = std::make_unique<claude::Conversation>();
    window->conversation->Load(one["conversation"]);
    window->id = nextId_++;
    windows_.push_back(std::move(window));
  }
}

void ChatPanel::Dev(const std::string& action) {
  if (windows_.empty()) return;
  ChatWindow& window = *windows_.front();
  if (action == "menu") {
    window.menuOpen = true;
  } else if (action == "discard") {
    Remove(0);
  } else if (action.rfind("rewind-canvas:", 0) == 0) {
    Rewind(0, static_cast<std::size_t>(std::atoi(action.c_str() + 14)), /*canvas=*/true);
  } else if (action.rfind("rewind:", 0) == 0) {
    Rewind(0, static_cast<std::size_t>(std::atoi(action.c_str() + 7)), /*canvas=*/false);
  } else if (action.rfind("say:", 0) == 0) {
    window.draft = action.substr(4);
    window.caret = window.draft.size();
    Send(window);
  }
}

std::vector<ChatPanel::Ref> ChatPanel::Refs() const {
  std::vector<Ref> out;
  for (const std::unique_ptr<ChatWindow>& window : windows_) {
    out.push_back({window->id, window->worldPos, window->width, window->height, window->madeBelow});
  }
  return out;
}

void ChatPanel::Restore(const std::vector<Ref>& refs) {
  const ChatWindow* typing =
      editing_ >= 0 && editing_ < static_cast<int>(windows_.size())
          ? windows_[static_cast<std::size_t>(editing_)].get()
          : nullptr;
  // Every conversation there is, open or set aside, by id.
  std::vector<std::unique_ptr<ChatWindow>> all;
  for (std::unique_ptr<ChatWindow>& window : windows_) all.push_back(std::move(window));
  for (std::unique_ptr<ChatWindow>& window : setAside_) all.push_back(std::move(window));
  windows_.clear();
  setAside_.clear();
  for (const Ref& ref : refs) {
    for (std::unique_ptr<ChatWindow>& window : all) {
      if (window == nullptr || window->id != ref.id) continue;
      window->worldPos = ref.worldPos;
      window->width = ref.width;
      window->height = ref.height;
      window->madeBelow = ref.madeBelow;
      windows_.push_back(std::move(window));
      break;
    }
  }
  // What the step does not have is set aside again, in case a redo wants it.
  for (std::unique_ptr<ChatWindow>& window : all) {
    if (window == nullptr) continue;
    window->conversation->Stop();
    setAside_.push_back(std::move(window));
  }
  editing_ = -1;
  for (std::size_t i = 0; i < windows_.size(); ++i) {
    if (windows_[i].get() == typing) editing_ = static_cast<int>(i);
  }
  drag_ = Drag::None;
  rewind_ = {};
  changed_ = true;
}

void ChatPanel::Clear() {
  // Each conversation stops its reply and joins its thread as it goes.
  windows_.clear();
  setAside_.clear();
  editing_ = -1;
  drag_ = Drag::None;
  rewind_ = {};
  canvasRewindDue_ = false;
  math_.clear();
  sizes_.clear();
}

void ChatPanel::Remove(std::size_t index) {
  if (index >= windows_.size()) return;
  // Set aside, not destroyed: an undo can bring it back. Its reply stops now,
  // so nothing goes on happening in a window nobody can see.
  std::unique_ptr<ChatWindow> gone = std::move(windows_[index]);
  windows_.erase(windows_.begin() + static_cast<long>(index));
  gone->conversation->Stop();
  gone->absorbed = false;
  gone->selected = false;
  gone->menuOpen = false;
  setAside_.push_back(std::move(gone));
  constexpr std::size_t kKept = 20;
  if (setAside_.size() > kKept) setAside_.erase(setAside_.begin());
  if (editing_ == static_cast<int>(index)) editing_ = -1;
  else if (editing_ > static_cast<int>(index)) --editing_;
  rewind_ = {};
  changed_ = true;
}

int ChatPanel::ItemAt(Vector2 point, const Viewport& viewport) const {
  for (std::size_t i = windows_.size(); i-- > 0;) {
    const Rectangle rect = ScreenRectOf(*windows_[i], viewport);
    if (CheckCollisionPointRec(point, rect) || PointOnRim(rect, point)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// ------------------------------------------------------------- geometry

float ChatPanel::InputHeight(const ChatWindow& window, FontStore& fonts) const {
  const float textW = window.width - 2.0f * kPad - 2.0f * kInputPad - kSendSize - 0.4f * kEm;
  const int   rows =
      std::clamp(static_cast<int>(Rows(fonts, window.draft, std::max(textW, kEm)).size()), 1,
                 kMaxInputRows);
  return static_cast<float>(rows) * LineH() + 2.0f * kInputPad;
}

ChatPanel::Parts ChatPanel::PartsOf(const ChatWindow& window, const Viewport& viewport,
                                    FontStore& fonts) const {
  Parts p;
  p.zoom = viewport.Zoom();
  const float z = p.zoom;
  p.window = ScreenRectOf(window, viewport);
  const Rectangle& w = p.window;
  p.header = {w.x, w.y, w.width, kHeader * z};
  const float inputH = InputHeight(window, fonts) * z;
  // The space under the input holds the resize handle, which is a fixed size on
  // screen: the input stays above it whatever the zoom.
  const float below = std::max(kPad * z, kRimGrabInside + 16.0f);
  p.input = {w.x + kPad * z, w.y + w.height - below - inputH, w.width - 2.0f * kPad * z, inputH};
  // Room above the input for a notice, and below the header for breath.
  const float top = w.y + kHeader * z + 0.4f * kEm * z;
  const float bottom = p.input.y - 1.6f * kEm * z;
  p.messages = {w.x + kPad * z, top, w.width - 2.0f * kPad * z, std::max(0.0f, bottom - top)};
  p.send = {p.input.x + p.input.width - (kSendSize + 0.25f * kEm) * z,
            p.input.y + p.input.height - (kSendSize + 0.25f * kEm) * z + 0.05f * kEm * z,
            kSendSize * z, kSendSize * z};
  // The effort slider: five stops on a track at the header's right.
  const float trackW = 6.5f * kEm * z;
  p.effort = {w.x + w.width - kPad * z - trackW, w.y + kHeader * z * 0.5f - 0.6f * kEm * z, trackW,
              1.2f * kEm * z};
  // The resize handle: a fixed size on screen, just inside the band the rim is
  // grabbed by, so the two never want the same pixel.
  constexpr float kHandle = 14.0f;
  p.corner = {w.x + w.width - kRimGrabInside - kHandle, w.y + w.height - kRimGrabInside - kHandle,
              kHandle, kHandle};
  // The model's name, first in the header: pressing it opens the menu.
  const float em = kEm * z;
  const float nameX = w.x + kPad * z;
  const claude::Model* model = claude::FindModel(window.conversation->model);
  const std::string    name = model != nullptr ? model->name : window.conversation->model;
  p.model = {nameX - 0.25f * em, w.y + 0.35f * em,
             text::Measure(fonts, Face::MainRegular, name, em * 0.85f) + 1.4f * em,
             kHeader * z - 0.7f * em};
  p.bar = {w.x + 0.6f * em, w.y + kHeader * z - 0.14f * em, w.width - 1.2f * em, 0.14f * em};
  p.menuRow = 2.4f * em;
  return p;
}

Rectangle ChatPanel::MenuRow(const Parts& parts, std::size_t row) const {
  const float em = kEm * parts.zoom;
  const float width = std::min(parts.window.width - 2.0f * kPad * parts.zoom, 22.0f * em);
  return {parts.window.x + kPad * parts.zoom,
          parts.header.y + parts.header.height + 0.3f * em + static_cast<float>(row) * parts.menuRow,
          width, parts.menuRow};
}

// ------------------------------------------------------------- typesetting

void ChatPanel::MathSize(const std::string& source, bool display, FontStore& fonts, float* w,
                         float* h, float* d) {
  const std::string key = (display ? "D" : "I") + source;
  auto              found = sizes_.find(key);
  if (found == sizes_.end()) {
    if (sizes_.size() > kCacheMax) sizes_.clear();
    const tex::Layout layout = tex::Render(source, fonts, kEm, display);
    found = sizes_.emplace(key, Size{layout.width, layout.height, layout.depth}).first;
  }
  *w = found->second.w;
  *h = found->second.h;
  *d = found->second.d;
}

const tex::Layout& ChatPanel::MathAt(const std::string& source, bool display, float emPx,
                                     FontStore& fonts) {
  if (std::fabs(emPx - mathEm_) > 0.01f || math_.size() > kCacheMax) {
    math_.clear();
    mathEm_ = emPx;
  }
  const std::string key = (display ? "D" : "I") + source;
  auto              found = math_.find(key);
  if (found == math_.end()) {
    found = math_.emplace(key, tex::Render(source, fonts, emPx, display)).first;
  }
  return found->second;
}

void ChatPanel::Layout(ChatWindow& window, FontStore& fonts) {
  const float width = window.width - 2.0f * kPad;
  if (window.laidWidth != width) {
    window.laid.clear();
    window.laidWidth = width;
  }
  const std::vector<claude::Turn>& turns = window.conversation->Turns();
  if (window.laid.size() > turns.size()) window.laid.resize(turns.size());

  const float space = text::Measure(fonts, Face::MainRegular, " ", kEm);
  const float lineH = LineH();

  const bool streaming = window.conversation->Busy();
  for (std::size_t t = 0; t < turns.size(); ++t) {
    const claude::Turn& turn = turns[t];
    const bool          partial = streaming && t + 1 == turns.size();
    if (t < window.laid.size() && window.laid[t].textSize == turn.text.size() &&
        window.laid[t].failed == turn.failed && window.laid[t].note == turn.note &&
        window.laid[t].partial == partial) {
      continue;
    }
    if (t >= window.laid.size()) window.laid.emplace_back();
    ChatWindow::Laid& laid = window.laid[t];
    laid = {};
    laid.textSize = turn.text.size();
    laid.failed = turn.failed;
    laid.note = turn.note;
    laid.partial = partial;

    const bool  user = turn.role == claude::Turn::Role::User;
    const float bubblePad = 0.5f * kEm;
    const float right = user ? width * 0.85f - 2.0f * bubblePad : width;
    std::vector<ChatWindow::Piece>& out = laid.pieces;
    float                           y = 0.0f;
    float                           widest = 0.0f;

    // One paragraph of words and formulas, broken into lines between `left`
    // and `right`.
    const auto flow = [&](const std::vector<Atom>& atoms, float left, Color tint) {
      std::vector<ChatWindow::Piece> line;
      float x = left;
      float ascent = text::Ascent(fonts, Face::MainRegular, kEm);
      float descent = text::Descent(fonts, Face::MainRegular, kEm);
      const auto finish = [&]() {
        const float baseline = y + std::max(ascent, lineH * 0.75f);
        for (ChatWindow::Piece& piece : line) {
          piece.y = baseline;
          widest = std::max(widest, piece.x + piece.w);
          out.push_back(std::move(piece));
        }
        line.clear();
        y = baseline + std::max(descent, lineH * 0.25f);
        x = left;
        ascent = text::Ascent(fonts, Face::MainRegular, kEm);
        descent = text::Descent(fonts, Face::MainRegular, kEm);
      };
      std::size_t i = 0;
      while (i < atoms.size()) {
        // A word is every atom up to the next space.
        std::size_t j = i + 1;
        while (j < atoms.size() && !atoms[j].spaceBefore) ++j;
        float wordW = 0.0f;
        std::vector<ChatWindow::Piece> pieces;
        float a = 0.0f;
        float d = 0.0f;
        for (std::size_t k = i; k < j; ++k) {
          const Atom&       atom = atoms[k];
          ChatWindow::Piece piece;
          piece.tint = tint;
          if (atom.kind == Atom::Kind::Math) {
            piece.kind = ChatWindow::Piece::Kind::Math;
            piece.text = atom.text;
            float h = 0.0f;
            float dd = 0.0f;
            MathSize(atom.text, false, fonts, &piece.w, &h, &dd);
            a = std::max(a, h);
            d = std::max(d, dd);
          } else if (atom.kind == Atom::Kind::Code) {
            piece.face = Face::Mono;
            piece.text = atom.text;
            piece.tint = kCodeTint;
            piece.h = kCodeEm;
            piece.w = text::Measure(fonts, Face::Mono, atom.text, kCodeEm);
          } else {
            piece.face = atom.bold ? Face::MainBold : Face::MainRegular;
            piece.text = atom.text;
            piece.w = text::Measure(fonts, piece.face, atom.text, kEm);
          }
          piece.x = wordW;
          wordW += piece.w;
          pieces.push_back(std::move(piece));
        }
        if (!line.empty() && x + space + wordW > right) finish();
        const float at = line.empty() ? x : x + space;
        for (ChatWindow::Piece& piece : pieces) {
          piece.x += at;
          line.push_back(std::move(piece));
        }
        x = at + wordW;
        ascent = std::max(ascent, a);
        descent = std::max(descent, d);
        i = j;
      }
      if (!line.empty()) finish();
    };

    // While a reply is still arriving, a formula whose closing dollar has not
    // come yet is held back rather than shown raw for a moment and then set.
    std::string body = turn.text;
    if (partial) {
      std::size_t dollars = 0;
      std::size_t lastSingle = std::string::npos;
      std::size_t displays = 0;
      std::size_t lastDisplay = std::string::npos;
      for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '$' || (i > 0 && body[i - 1] == '\\')) continue;
        if (i + 1 < body.size() && body[i + 1] == '$') {
          ++displays;
          lastDisplay = i;
          ++i;
        } else {
          ++dollars;
          lastSingle = i;
        }
      }
      std::size_t cut = std::string::npos;
      if (displays % 2 == 1) cut = lastDisplay;
      if (dollars % 2 == 1) cut = std::min(cut, lastSingle);
      if (cut != std::string::npos) body.resize(cut);
    }

    // Blocks: paragraphs, list items, display mathematics, fenced code.
    std::vector<std::string> lines;
    {
      std::size_t from = 0;
      while (true) {
        const std::size_t nl = body.find('\n', from);
        lines.push_back(body.substr(from, nl == std::string::npos ? std::string::npos
                                                                  : nl - from));
        if (nl == std::string::npos) break;
        from = nl + 1;
      }
    }
    bool       started = false;
    const auto gap = [&](float amount) {
      if (started) y += amount;
      started = true;
    };
    for (std::size_t i = 0; i < lines.size();) {
      const std::string line = Trim(lines[i]);
      if (line.empty()) {
        ++i;
        continue;
      }
      if (line.rfind("```", 0) == 0) {
        gap(0.4f * lineH);
        const float top = y;
        ++i;
        while (i < lines.size() && Trim(lines[i]).rfind("```", 0) != 0) {
          for (const std::string& row :
               text::Wrap(fonts, Face::Mono, lines[i], kCodeEm, right - kEm)) {
            ChatWindow::Piece piece;
            piece.face = Face::Mono;
            piece.text = row;
            piece.tint = kCodeTint;
            piece.x = 0.5f * kEm;
            piece.y = y + kCodeEm;
            piece.w = text::Measure(fonts, Face::Mono, row, kCodeEm);
            out.push_back(piece);
            y += kCodeEm * 1.35f;
          }
          ++i;
        }
        ++i;  // the closing fence
        ChatWindow::Piece box;
        box.kind = ChatWindow::Piece::Kind::Box;
        box.x = 0.0f;
        box.y = top - 0.2f * kEm;
        box.w = right;
        box.h = y - top + 0.5f * kEm;
        box.tint = kBubble;
        out.insert(out.begin(), box);
        y += 0.3f * kEm;
        widest = std::max(widest, right);
        continue;
      }
      if (line.rfind("$$", 0) == 0 || line.rfind("\\[", 0) == 0) {
        const bool        dollars = line.rfind("$$", 0) == 0;
        const std::string close = dollars ? "$$" : "\\]";
        std::string       source = line.substr(2);
        std::size_t       end = source.find(close);
        ++i;
        while (end == std::string::npos && i < lines.size()) {
          source += " " + lines[i];
          end = source.find(close);
          ++i;
        }
        if (end != std::string::npos) source = source.substr(0, end);
        gap(0.4f * lineH);
        ChatWindow::Piece piece;
        piece.kind = ChatWindow::Piece::Kind::Math;
        piece.display = true;
        piece.text = Trim(source);
        piece.tint = kFg;
        float h = 0.0f;
        float d = 0.0f;
        MathSize(piece.text, true, fonts, &piece.w, &h, &d);
        piece.x = std::max(0.0f, (right - piece.w) * 0.5f);
        piece.y = y + h + 0.2f * kEm;
        y = piece.y + d + 0.2f * kEm;
        widest = std::max(widest, piece.x + piece.w);
        out.push_back(std::move(piece));
        continue;
      }
      const bool bullet = line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0;
      std::size_t numberEnd = 0;
      while (numberEnd < line.size() && std::isdigit(static_cast<unsigned char>(line[numberEnd]))) {
        ++numberEnd;
      }
      const bool numbered = numberEnd > 0 && line.compare(numberEnd, 2, ". ") == 0;
      if (bullet || numbered) {
        gap(0.15f * lineH);
        const float indent = 1.3f * kEm;
        // A bullet is drawn rather than set: the roman face has no glyph for one.
        ChatWindow::Piece mark;
        const float baseline = y + std::max(text::Ascent(fonts, Face::MainRegular, kEm), lineH * 0.75f);
        if (bullet) {
          mark.kind = ChatWindow::Piece::Kind::Dot;
          mark.x = 0.5f * kEm;
          mark.y = baseline - 0.32f * kEm;
          mark.w = 0.13f * kEm;
        } else {
          mark.text = line.substr(0, numberEnd + 1);
          mark.w = text::Measure(fonts, Face::MainRegular, mark.text, kEm);
          mark.y = baseline;
        }
        mark.tint = kMuted;
        out.push_back(mark);
        flow(Inline(Trim(line.substr(bullet ? 2 : numberEnd + 2))), indent, kFg);
        ++i;
        continue;
      }
      if (line[0] == '#') {
        gap(0.5f * lineH);
        std::vector<Atom> atoms = Inline(Trim(line.substr(line.find_first_not_of('#'))));
        for (Atom& atom : atoms) atom.bold = true;
        flow(atoms, 0.0f, kFg);
        ++i;
        continue;
      }
      // A paragraph: this line and every plain one after it, up to a blank.
      std::string paragraph = line;
      ++i;
      while (i < lines.size()) {
        const std::string next = Trim(lines[i]);
        if (next.empty() || next.rfind("```", 0) == 0 || next.rfind("$$", 0) == 0 ||
            next.rfind("\\[", 0) == 0 || next.rfind("- ", 0) == 0 || next.rfind("* ", 0) == 0 ||
            next[0] == '#') {
          break;
        }
        paragraph += " " + next;
        ++i;
      }
      gap(0.5f * lineH);
      flow(Inline(paragraph), 0.0f, kFg);
    }

    if (!turn.note.empty()) {
      gap(0.3f * lineH);
      ChatWindow::Piece note;
      note.text = turn.note;
      note.tint = kNoteFg;
      note.w = text::Measure(fonts, Face::MainItalic, note.text, kEm);
      note.face = Face::MainItalic;
      note.y = y + lineH * 0.75f;
      out.push_back(note);
      y += lineH;
      widest = std::max(widest, note.w);
    }
    laid.height = y;

    // Your own words sit in a bubble at the right, so the two voices are told
    // apart at a glance.
    if (user) {
      const float bubbleW = std::min(widest, right) + 2.0f * bubblePad;
      const float shift = width - bubbleW + bubblePad;
      for (ChatWindow::Piece& piece : out) {
        piece.x += shift;
        piece.y += bubblePad;
      }
      ChatWindow::Piece box;
      box.kind = ChatWindow::Piece::Kind::Box;
      box.x = width - bubbleW;
      box.y = 0.0f;
      box.w = bubbleW;
      box.h = y + 2.0f * bubblePad;
      box.tint = kBubble;
      out.insert(out.begin(), box);
      laid.height = box.h;
    }
  }
}

// ------------------------------------------------------------- input

void ChatPanel::Send(ChatWindow& window) {
  claude::Conversation& conversation = *window.conversation;
  if (conversation.Busy()) return;
  const std::string text = Trim(window.draft);
  if (text.empty()) return;
  const std::string key = settings::ApiKey();
  if (key.empty()) {
    window.notice = "No API key yet: press Escape, then Paste Claude API key.";
    return;
  }
  claude::CanvasView view = view_ ? view_() : claude::CanvasView{};
  window.lookValid = pointed_ && pointed_(&window.lookWorld, &window.lookKind);
  if (window.lookValid) {
    view.status += "His pointer was on a " + window.lookKind +
                   " window when he sent this: that is what look would show.\n";
  }
  conversation.Send(key, text, view, prompt::System(), prompt::Tools(),
                    snapshot_ ? snapshot_() : std::string());
  changed_ = true;
  window.draft.clear();
  window.caret = 0;
  window.notice.clear();
  window.following = true;
}

void ChatPanel::SetEffortAt(ChatWindow& window, const Parts& parts, float x) {
  const float t = (x - parts.effort.x) / std::max(parts.effort.width, 1.0f);
  const int   stop = std::clamp(static_cast<int>(std::lround(t * (claude::kEffortCount - 1))), 0,
                                claude::kEffortCount - 1);
  window.conversation->effort = claude::kEfforts[stop];
}

void ChatPanel::PlaceCaret(ChatWindow& window, const Parts& parts, Vector2 point,
                           FontStore& fonts) {
  const float z = parts.zoom;
  const float textW = (parts.input.width - parts.send.width) / z - 2.0f * kInputPad - 0.4f * kEm;
  const auto  rows = Rows(fonts, window.draft, std::max(textW, kEm));
  const int   first = std::max(0, static_cast<int>(rows.size()) - kMaxInputRows);
  int         row = first + static_cast<int>((point.y - parts.input.y - kInputPad * z) / (LineH() * z));
  row = std::clamp(row, 0, static_cast<int>(rows.size()) - 1);
  const auto [begin, end] = rows[static_cast<std::size_t>(row)];
  const float x = (point.x - parts.input.x) / z - kInputPad;
  std::size_t at = begin;
  while (at < end && window.draft[at] != '\n') {
    const std::size_t next = NextCodepointAt(window.draft, at);
    const float       mid =
        (text::Measure(fonts, Face::MainRegular,
                       std::string_view(window.draft).substr(begin, at - begin), kEm) +
         text::Measure(fonts, Face::MainRegular,
                       std::string_view(window.draft).substr(begin, next - begin), kEm)) *
        0.5f;
    if (mid > x) break;
    at = next;
  }
  window.caret = at;
}

bool ChatPanel::HandleInput(const Viewport& viewport, FontStore& fonts) {
  const Vector2 pointer = GetMousePosition();

  // A gesture in flight owns the pointer until the button comes up.
  if (drag_ != Drag::None) {
    if (dragIndex_ >= windows_.size()) {
      drag_ = Drag::None;
      return false;
    }
    ChatWindow&   window = *windows_[dragIndex_];
    const Vector2 world = viewport.ToWorld(pointer);
    const bool    released = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    if (drag_ == Drag::Move) {
      const Vector2 to = {world.x - dragGrab_.x, world.y - dragGrab_.y};
      if (!dragMoved_ && std::hypot(to.x - window.worldPos.x, to.y - window.worldPos.y) *
                                 viewport.Zoom() > 3.0f) {
        dragMoved_ = true;
      }
      if (dragMoved_) window.worldPos = to;
      window.absorbed = dragMoved_ && OverDropTarget(pointer);
      if (released) {
        drag_ = Drag::None;
        if (window.absorbed) {
          Remove(dragIndex_);
        } else {
          window.worldPos = SnapToGrid(window.worldPos, viewport.Zoom());
        }
        dragMoved_ = false;
        changed_ = true;
      }
    } else if (drag_ == Drag::Resize) {
      window.width = std::max(kMinWidth, world.x + dragGrab_.x - window.worldPos.x);
      window.height = std::max(kMinHeight, world.y + dragGrab_.y - window.worldPos.y);
      if (released) {
        drag_ = Drag::None;
        changed_ = true;
      }
    } else if (drag_ == Drag::Effort) {
      SetEffortAt(window, PartsOf(window, viewport, fonts), pointer.x);
      if (released) {
        drag_ = Drag::None;
        changed_ = true;
      }
    }
    return true;
  }

  const bool  pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
  const float wheel = GetMouseWheelMoveV().y;

  // An open model menu answers the press, wherever it lands: on a row it
  // chooses, anywhere else it closes.
  for (std::size_t i = 0; i < windows_.size() && pressed; ++i) {
    ChatWindow& window = *windows_[i];
    if (!window.menuOpen) continue;
    window.menuOpen = false;
    const Parts parts = PartsOf(window, viewport, fonts);
    const std::vector<claude::Model>& models = claude::Models();
    for (std::size_t row = 0; row < models.size(); ++row) {
      if (!CheckCollisionPointRec(pointer, MenuRow(parts, row))) continue;
      window.conversation->SetModel(models[row].id);
      changed_ = true;
      return true;
    }
    if (CheckCollisionPointRec(pointer, parts.window)) return true;
  }

  // Rewinding: the button, then the question it asks.
  if (pressed && rewind_.window >= 0) {
    const std::size_t w = static_cast<std::size_t>(rewind_.window);
    if (!rewind_.asking && CheckCollisionPointRec(pointer, rewind_.chip)) {
      rewind_.asking = true;
      return true;
    }
    if (rewind_.asking && CheckCollisionPointRec(pointer, rewind_.words)) {
      Rewind(w, rewind_.turn, /*canvas=*/false);
      return true;
    }
    if (rewind_.asking && CheckCollisionPointRec(pointer, rewind_.canvas)) {
      Rewind(w, rewind_.turn, /*canvas=*/true);
      return true;
    }
    rewind_.asking = false;
  }

  for (std::size_t i = windows_.size(); i-- > 0;) {
    ChatWindow&  window = *windows_[i];
    const Parts  parts = PartsOf(window, viewport, fonts);
    const bool   over = CheckCollisionPointRec(pointer, parts.window);
    const bool   onRim = PointOnRim(parts.window, pointer);
    if (!over && !onRim) continue;

    // The wheel scrolls what has been said: a window that scrolls inside is
    // one whose wheel is its own.
    if (wheel != 0.0f && over) {
      window.scroll -= wheel * 3.0f * LineH();
      window.following = false;
      return true;
    }
    if (!pressed) return true;

    const Vector2 world = viewport.ToWorld(pointer);
    if (CheckCollisionPointRec(pointer, parts.corner)) {
      drag_ = Drag::Resize;
      dragIndex_ = i;
      dragGrab_ = {window.worldPos.x + window.width - world.x,
                   window.worldPos.y + window.height - world.y};
      return true;
    }
    if (onRim) {
      drag_ = Drag::Move;
      dragIndex_ = i;
      dragGrab_ = {world.x - window.worldPos.x, world.y - window.worldPos.y};
      dragMoved_ = false;
      return true;
    }
    editing_ = static_cast<int>(i);
    if (CheckCollisionPointRec(pointer, parts.model)) {
      // Another model only between replies: a reply in flight is that model's.
      if (!window.conversation->Busy()) window.menuOpen = true;
      return true;
    }
    if (CheckCollisionPointRec(pointer, parts.effort)) {
      drag_ = Drag::Effort;
      dragIndex_ = i;
      SetEffortAt(window, parts, pointer.x);
      return true;
    }
    if (CheckCollisionPointRec(pointer, parts.send)) {
      if (window.conversation->Busy()) window.conversation->Stop();
      else Send(window);
      return true;
    }
    if (CheckCollisionPointRec(pointer, parts.input)) PlaceCaret(window, parts, pointer, fonts);
    return true;
  }
  // A press anywhere else is somewhere else: the caret leaves.
  if (pressed) editing_ = -1;
  return false;
}

void ChatPanel::HandleKeys(FontStore& fonts) {
  if (editing_ < 0 || editing_ >= static_cast<int>(windows_.size())) {
    editing_ = -1;
    return;
  }
  ChatWindow&  window = *windows_[static_cast<std::size_t>(editing_)];
  std::string& s = window.draft;
  std::size_t& at = window.caret;
  at = std::min(at, s.size());
  const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
  const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
  bool       typed = false;

  for (int c = GetCharPressed(); c > 0; c = GetCharPressed()) {
    if (c < 32 || ctrl) continue;
    std::string bytes;
    PutUtf8(c, &bytes);
    s.insert(at, bytes);
    at += bytes.size();
    typed = true;
  }
  if (Triggered(KEY_ENTER) || Triggered(KEY_KP_ENTER)) {
    if (shift) {
      s.insert(at, "\n");
      ++at;
      typed = true;
    } else {
      Send(window);
      return;
    }
  }
  const auto wordBack = [&](std::size_t from) {
    while (from > 0 && s[from - 1] == ' ') --from;
    while (from > 0 && s[from - 1] != ' ' && s[from - 1] != '\n') --from;
    return from;
  };
  const auto wordForward = [&](std::size_t from) {
    while (from < s.size() && s[from] == ' ') ++from;
    while (from < s.size() && s[from] != ' ' && s[from] != '\n') ++from;
    return from;
  };
  if (Triggered(KEY_BACKSPACE) && at > 0) {
    const std::size_t from = ctrl ? wordBack(at) : PrevCodepoint(s, at);
    s.erase(from, at - from);
    at = from;
    typed = true;
  }
  if (Triggered(KEY_DELETE) && at < s.size()) {
    const std::size_t to = ctrl ? wordForward(at) : NextCodepointAt(s, at);
    s.erase(at, to - at);
    typed = true;
  }
  if (Triggered(KEY_LEFT)) at = ctrl ? wordBack(at) : PrevCodepoint(s, at);
  if (Triggered(KEY_RIGHT)) at = ctrl ? wordForward(at) : NextCodepointAt(s, at);
  if (Triggered(KEY_HOME)) {
    while (at > 0 && s[at - 1] != '\n') --at;
  }
  if (Triggered(KEY_END)) {
    while (at < s.size() && s[at] != '\n') ++at;
  }
  if (Triggered(KEY_UP) || Triggered(KEY_DOWN)) {
    // To the same place in the row above or below, as near as the letters allow.
    const float textW = window.width - 2.0f * kPad - 2.0f * kInputPad - kSendSize - 0.4f * kEm;
    const auto  rows = Rows(fonts, s, std::max(textW, kEm));
    int         row = 0;
    for (std::size_t r = 0; r < rows.size(); ++r) {
      if (at >= rows[r].first && at <= rows[r].second) row = static_cast<int>(r);
    }
    const int to = row + (Triggered(KEY_UP) ? -1 : 1);
    if (to >= 0 && to < static_cast<int>(rows.size())) {
      const float x = text::Measure(fonts, Face::MainRegular,
                                    std::string_view(s).substr(rows[static_cast<std::size_t>(row)].first,
                                                               at - rows[static_cast<std::size_t>(row)].first),
                                    kEm);
      const auto [begin, end] = rows[static_cast<std::size_t>(to)];
      std::size_t pos = begin;
      while (pos < end && s[pos] != '\n') {
        const std::size_t next = NextCodepointAt(s, pos);
        if (text::Measure(fonts, Face::MainRegular, std::string_view(s).substr(begin, next - begin),
                          kEm) > x) {
          break;
        }
        pos = next;
      }
      at = pos;
    }
  }
  if (ctrl && IsKeyPressed(KEY_V)) {
    if (const char* clip = GetClipboardText()) {
      std::string paste;
      for (const char* p = clip; *p != '\0'; ++p) {
        if (*p != '\r') paste.push_back(*p);
      }
      s.insert(at, paste);
      at += paste.size();
      typed = true;
    }
  }
  if (typed) window.notice.clear();
}

void ChatPanel::Update() {
  for (const std::unique_ptr<ChatWindow>& window : windows_) {
    claude::Conversation& conversation = *window->conversation;
    const bool            was = conversation.Busy();
    conversation.Poll();
    // A reply finished: said in the log, so a run with nobody watching can be
    // read afterwards.
    if (was && !conversation.Busy() && !conversation.Turns().empty()) {
      changed_ = true;
      const claude::Turn& reply = conversation.Turns().back();
      TraceLog(LOG_INFO, "CLAUDE: replied (%s%s): %s", reply.note.c_str(),
               reply.failed ? ", failed" : "", reply.text.c_str());
    }
  }
}

// ------------------------------------------------------------- drawing

void ChatPanel::DrawWindow(std::size_t index, const Viewport& viewport, FontStore& fonts,
                           bool hovered) {
  ChatWindow& window = *windows_[index];
  const Parts p = PartsOf(window, viewport, fonts);
  const float z = p.zoom;
  const float em = kEm * z;
  const claude::Conversation& conversation = *window.conversation;

  DrawCard(p.window, z, kCodeBg, Fade(kCodeFg, 0.12f), 1.0f);

  // --- the header: who, which model, what it has cost, and the effort -------
  {
    const float baseline = p.header.y + p.header.height * 0.5f +
                           text::Ascent(fonts, Face::MainRegular, em) * 0.35f;
    float x = p.header.x + kPad * z;
    // The model's name is a button, and looks like one when reached for.
    const claude::Model* model = claude::FindModel(conversation.model);
    const std::string    name = model != nullptr ? model->name : conversation.model;
    const bool overModel = CheckCollisionPointRec(hover::TopPointer(), p.model) && !conversation.Busy();
    if (overModel || window.menuOpen) {
      DrawRectangleRounded(p.model, 0.4f, 8, kBubble);
    }
    text::Draw(fonts, Face::MainRegular, name, {x, baseline}, em * 0.85f,
               overModel || window.menuOpen ? kFg : kMuted);
    {
      const float cx = x + text::Measure(fonts, Face::MainRegular, name, em * 0.85f) + 0.55f * em;
      const float cy = baseline - 0.28f * em;
      const float arm = 0.22f * em;
      const float t = std::max(1.0f, 0.07f * em);
      DrawLineEx({cx - arm, cy - arm * 0.5f}, {cx, cy + arm * 0.5f}, t, kMuted);
      DrawLineEx({cx, cy + arm * 0.5f}, {cx + arm, cy - arm * 0.5f}, t, kMuted);
    }
    x = p.model.x + p.model.width + 0.6f * em;
    if (conversation.ContextTokens() > 0) {
      const std::string detail =
          Tokens(conversation.ContextTokens()) + " tokens     " + Dollars(conversation.Dollars());
      text::Draw(fonts, Face::MainRegular, detail, {x, baseline}, em * 0.85f, kMuted);
    }

    // The effort slider, always there: five stops, the chosen one large.
    const int   chosen = EffortIndex(conversation.effort);
    const float cy = p.effort.y + p.effort.height * 0.5f;
    const float left = p.effort.x + 0.3f * em;
    const float right = p.effort.x + p.effort.width - 0.3f * em;
    const std::string label = conversation.effort;
    const float labelW = text::Measure(fonts, Face::MainRegular, label, em * 0.8f);
    text::Draw(fonts, Face::MainRegular, label, {p.effort.x - labelW - 0.5f * em, baseline},
               em * 0.8f, kMuted);
    DrawLineEx({left, cy}, {right, cy}, std::max(1.0f, 0.08f * em), Fade(kMuted, 0.6f));
    for (int i = 0; i < claude::kEffortCount; ++i) {
      const float sx = left + (right - left) * static_cast<float>(i) /
                                  static_cast<float>(claude::kEffortCount - 1);
      if (i == chosen) DrawCircleV({sx, cy}, 0.32f * em, kAccentFg);
      else DrawCircleV({sx, cy}, 0.14f * em, i < chosen ? kAccentFg : kMuted);
    }
    DrawLineEx({p.window.x, p.header.y + p.header.height},
               {p.window.x + p.window.width, p.header.y + p.header.height},
               std::max(1.0f, 0.05f * em), Fade(kCodeFg, 0.08f));

    // How full the context is, under the header: a fraction of what the model
    // can hold, turning amber and then red as it fills.
    const long  room = model != nullptr ? model->context : 1000000;
    const float full = std::clamp(static_cast<float>(conversation.ContextTokens()) /
                                      static_cast<float>(std::max(room, 1L)),
                                  0.0f, 1.0f);
    if (full > 0.0f) {
      const Color fill = full > 0.9f   ? kNoteFg
                         : full > 0.7f ? Color{230, 180, 90, 255}
                                       : kAccentFg;
      DrawRectangleRec(p.bar, Fade(kMuted, 0.18f));
      DrawRectangleRec({p.bar.x, p.bar.y, std::max(1.0f, p.bar.width * full), p.bar.height}, fill);
    }
  }

  // --- what has been said ---------------------------------------------------
  Layout(window, fonts);
  float content = 0.0f;
  for (std::size_t t = 0; t < window.laid.size(); ++t) {
    content += window.laid[t].height + (t + 1 < window.laid.size() ? kTurnGap : 0.0f);
  }
  const bool indicator = conversation.Busy() &&
                         (conversation.Thinking() || conversation.Working() ||
                          conversation.Turns().back().text.empty());
  if (indicator) content += kTurnGap + LineH();
  const float view = p.messages.height / z;
  const float most = std::max(0.0f, content - view);
  if (window.following) window.scroll = most;
  window.scroll = std::clamp(window.scroll, 0.0f, most);
  if (window.scroll >= most - 1.0f) window.following = true;

  const int sx = std::max(0, static_cast<int>(p.messages.x));
  const int sy = std::max(0, static_cast<int>(p.messages.y));
  const int sw = std::min(GetScreenWidth(), static_cast<int>(p.messages.x + p.messages.width)) - sx;
  const int sh = std::min(GetScreenHeight(), static_cast<int>(p.messages.y + p.messages.height)) - sy;
  if (sw > 0 && sh > 0) {
    BeginScissorMode(sx, sy, sw, sh);
    float top = 0.0f;
    const std::vector<claude::Turn>& turns = conversation.Turns();
    for (std::size_t t = 0; t < window.laid.size(); ++t) {
      const ChatWindow::Laid& laid = window.laid[t];
      const float             y0 = top - window.scroll;
      top += laid.height + kTurnGap;
      // Only what is in view is drawn.
      if (y0 > view || y0 + laid.height < 0.0f) continue;

      // Your message offers to be rewound to, while nothing is coming back.
      if (t < turns.size() && turns[t].role == claude::Turn::Role::User && !laid.pieces.empty() &&
          !conversation.Busy()) {
        const ChatWindow::Piece& bubble = laid.pieces.front();
        const Rectangle box = {p.messages.x + bubble.x * z, p.messages.y + (y0 + bubble.y) * z,
                               bubble.w * z, bubble.h * z};
        const Vector2   aim = hover::TopPointer();
        const bool      mine = rewind_.window == static_cast<int>(index) && rewind_.turn == t;
        const bool      reaching = CheckCollisionPointRec(aim, box) ||
                              (mine && (CheckCollisionPointRec(aim, rewind_.chip) ||
                                        (rewind_.asking && (CheckCollisionPointRec(aim, rewind_.words) ||
                                                            CheckCollisionPointRec(aim, rewind_.canvas)))));
        if (reaching || (mine && rewind_.asking)) {
          const float small = em * 0.75f;
          const auto  chipFor = [&](const char* words, float y) {
            const float w = text::Measure(fonts, Face::MainRegular, words, small) + 0.9f * em;
            return Rectangle{box.x - w - 0.35f * em, y, w, 1.5f * em};
          };
          const auto drawChip = [&](const Rectangle& r, const char* words) {
            const bool over = CheckCollisionPointRec(aim, r);
            DrawRectangleRounded(r, 0.5f, 8, over ? kRimActive : kBubble);
            text::Draw(fonts, Face::MainRegular, words,
                       {r.x + 0.45f * em, r.y + r.height * 0.5f + text::Ascent(fonts, Face::MainRegular, small) * 0.4f},
                       small, kFg);
          };
          rewind_.window = static_cast<int>(index);
          rewind_.turn = t;
          if (!(mine && rewind_.asking)) rewind_.asking = false;
          if (rewind_.asking) {
            rewind_.words = chipFor("rewind the conversation", box.y);
            rewind_.canvas = chipFor("rewind it and the canvas", box.y + 1.7f * em);
            drawChip(rewind_.words, "rewind the conversation");
            drawChip(rewind_.canvas, "rewind it and the canvas");
          } else {
            rewind_.chip = chipFor("rewind", box.y);
            drawChip(rewind_.chip, "rewind");
          }
        } else if (mine) {
          rewind_ = {};
        }
      }
      for (const ChatWindow::Piece& piece : laid.pieces) {
        const float py = y0 + piece.y;
        if (piece.kind != ChatWindow::Piece::Kind::Box && (py < -3.0f * kEm || py > view + 3.0f * kEm)) {
          continue;
        }
        const Vector2 at = {p.messages.x + piece.x * z, p.messages.y + py * z};
        if (piece.kind == ChatWindow::Piece::Kind::Dot) {
          DrawCircleV(at, piece.w * z, piece.tint);
        } else if (piece.kind == ChatWindow::Piece::Kind::Box) {
          const Rectangle box = {at.x, at.y, piece.w * z, piece.h * z};
          const float     roundness = std::clamp(0.6f * em / std::max(1.0f, std::min(box.width, box.height)), 0.0f, 1.0f);
          DrawRectangleRounded(box, roundness, 8, piece.tint);
        } else if (piece.kind == ChatWindow::Piece::Kind::Math) {
          tex::DrawLayout(MathAt(piece.text, piece.display, em, fonts), fonts, at, piece.tint);
        } else {
          const float size = piece.face == Face::Mono ? kCodeEm * z : em;
          text::Draw(fonts, piece.face, piece.text, at, size, piece.tint);
        }
      }
    }
    if (indicator) {
      // Thinking is not shown, only that it is happening.
      const int   dots = static_cast<int>(GetTime() * 2.5) % 4;
      std::string words = conversation.Working() ? "working" : "thinking";
      for (int i = 0; i < dots; ++i) words += ".";
      const float y = (top - window.scroll) * z + text::Ascent(fonts, Face::MainItalic, em);
      text::Draw(fonts, Face::MainItalic, words, {p.messages.x, p.messages.y + y}, em, kMuted);
    }
    EndScissorMode();
  }
  // A scroll bar when there is more than fits.
  if (most > 0.0f) {
    const float trackH = p.messages.height;
    const float barH = std::max(1.5f * em, trackH * view / content);
    const float barY = p.messages.y + (trackH - barH) * (window.scroll / most);
    DrawRectangleRounded({p.window.x + p.window.width - 0.45f * em, barY, 0.22f * em, barH}, 1.0f, 6,
                         Fade(kMuted, 0.5f));
  }

  // --- the input ------------------------------------------------------------
  DrawRectangleRounded(p.input, std::clamp(0.7f * em / std::max(1.0f, p.input.height), 0.0f, 1.0f), 8,
                       kBubble);
  const float textW = (p.input.width - p.send.width) / z - 2.0f * kInputPad - 0.4f * kEm;
  const auto  rows = Rows(fonts, window.draft, std::max(textW, kEm));
  const int   first = std::max(0, static_cast<int>(rows.size()) - kMaxInputRows);
  const float textX = p.input.x + kInputPad * z;
  const float ascent = text::Ascent(fonts, Face::MainRegular, em);
  const bool  editing = editing_ >= 0 && windows_[static_cast<std::size_t>(editing_)].get() == &window;
  // The rows partition the draft, so the caret is in the last row that starts
  // at or before it.
  int caretRow = 0;
  for (std::size_t r = 0; r < rows.size(); ++r) {
    if (rows[r].first <= window.caret) caretRow = static_cast<int>(r);
  }
  for (int r = first; r < static_cast<int>(rows.size()); ++r) {
    const auto [begin, end] = rows[static_cast<std::size_t>(r)];
    std::string_view shown = std::string_view(window.draft).substr(begin, end - begin);
    if (!shown.empty() && shown.back() == '\n') shown.remove_suffix(1);
    const float y = p.input.y + kInputPad * z + static_cast<float>(r - first) * LineH() * z + ascent;
    text::Draw(fonts, Face::MainRegular, shown, {textX, y}, em, kFg);
    // The caret, blinking, in the row it is in.
    if (editing && r == caretRow && std::fmod(GetTime(), 1.0) < 0.6) {
      const float cx = textX + text::Measure(fonts, Face::MainRegular,
                                             std::string_view(window.draft).substr(begin, window.caret - begin), em);
      DrawLineEx({cx, y - ascent}, {cx, y + 0.25f * em}, std::max(1.0f, 0.07f * em), kCaret);
    }
  }
  // Send, or stop while a reply is coming.
  {
    const Vector2 c = {p.send.x + p.send.width * 0.5f, p.send.y + p.send.height * 0.5f};
    const float   r = p.send.width * 0.5f;
    const bool    busy = conversation.Busy();
    const bool    ready = busy || !Trim(window.draft).empty();
    const bool    overSend = CheckCollisionPointRec(hover::TopPointer(), p.send);
    DrawCircleV(c, r, ready ? (overSend ? kRimActive : kAccentFg) : Fade(kMuted, 0.4f));
    if (busy) {
      const float s = r * 0.7f;
      DrawRectangleRec({c.x - s * 0.5f, c.y - s * 0.5f, s, s}, kCodeBg);
    } else {
      const float a = r * 0.5f;
      const float t = std::max(1.5f, 0.12f * em);
      DrawLineEx({c.x, c.y + a}, {c.x, c.y - a}, t, kCodeBg);
      DrawLineEx({c.x - a * 0.8f, c.y - a * 0.2f}, {c.x, c.y - a}, t, kCodeBg);
      DrawLineEx({c.x + a * 0.8f, c.y - a * 0.2f}, {c.x, c.y - a}, t, kCodeBg);
    }
  }
  if (!window.notice.empty()) {
    text::Draw(fonts, Face::MainItalic, window.notice,
               {p.input.x + 0.2f * em, p.input.y - 0.45f * em}, em * 0.85f, kNoteFg);
  }

  // The model menu, over everything else in the window: modelled on Claude
  // Code's -- each model's name and what it is for, a tick on the one in use --
  // with what it costs per million tokens, since that is what choosing one
  // decides.
  if (window.menuOpen) {
    const std::vector<claude::Model>& models = claude::Models();
    const Rectangle first = MenuRow(p, 0);
    const Rectangle panel = {first.x - 0.3f * em, first.y - 0.3f * em, first.width + 0.6f * em,
                             p.menuRow * static_cast<float>(models.size()) + 0.6f * em};
    DrawRectangleRounded(panel, 0.08f, 8, kChromeBg);
    DrawRectangleRoundedLinesEx(panel, 0.08f, 8, 1.0f, Fade(kCodeFg, 0.15f));
    const Vector2 aim = hover::TopPointer();
    for (std::size_t row = 0; row < models.size(); ++row) {
      const claude::Model& model = models[row];
      const Rectangle      r = MenuRow(p, row);
      if (CheckCollisionPointRec(aim, r)) DrawRectangleRounded(r, 0.25f, 8, kChromeHover);
      const float nameBase = r.y + 0.95f * em;
      text::Draw(fonts, Face::MainBold, model.name, {r.x + 1.6f * em, nameBase}, em * 0.9f, kFg);
      text::Draw(fonts, Face::MainRegular, model.blurb, {r.x + 1.6f * em, nameBase + 1.0f * em},
                 em * 0.75f, kMuted);
      char price[48];
      std::snprintf(price, sizeof(price), "$%g / $%g", model.price.input, model.price.output);
      const float priceW = text::Measure(fonts, Face::MainRegular, price, em * 0.75f);
      text::Draw(fonts, Face::MainRegular, price, {r.x + r.width - priceW - 0.5f * em, nameBase},
                 em * 0.75f, kMuted);
      if (model.id == conversation.model) {
        const float cx = r.x + 0.75f * em;
        const float cy = nameBase - 0.3f * em;
        const float t = std::max(1.5f, 0.1f * em);
        DrawLineEx({cx - 0.3f * em, cy}, {cx - 0.08f * em, cy + 0.25f * em}, t, kAccentFg);
        DrawLineEx({cx - 0.08f * em, cy + 0.25f * em}, {cx + 0.35f * em, cy - 0.3f * em}, t, kAccentFg);
      }
    }
  }

  // The resize handle and the rim, when the window is being reached for.
  if (hovered) {
    const Rectangle& h = p.corner;
    for (int k = 1; k <= 3; ++k) {
      const float o = h.width * static_cast<float>(k) / 3.0f;
      DrawLineEx({h.x + h.width, h.y + h.height - o}, {h.x + h.width - o, h.y + h.height}, 1.5f,
                 Fade(kCodeFg, 0.45f));
    }
  }
}

void ChatPanel::Draw(const Viewport& viewport, FontStore& fonts) {
  // Which one the pointer is on: the topmost, since they are drawn in order.
  const Vector2 aim = hover::TopPointer();
  int           top = -1;
  for (std::size_t i = windows_.size(); i-- > 0;) {
    const Rectangle rect = ScreenRectOf(*windows_[i], viewport);
    if (CheckCollisionPointRec(aim, rect) || PointOnRim(rect, aim)) {
      top = static_cast<int>(i);
      break;
    }
  }
  for (std::size_t i = 0; i < windows_.size(); ++i) {
    ChatWindow& window = *windows_[i];
    // Absorbed by the drop target: gone from the canvas until you pull off it.
    if (window.absorbed) continue;
    const Rectangle rect = ScreenRectOf(window, viewport);
    if (rect.x > GetScreenWidth() || rect.y > GetScreenHeight() || rect.x + rect.width < 0.0f ||
        rect.y + rect.height < 0.0f) {
      continue;
    }
    const bool carried = drag_ == Drag::Move && dragMoved_ && i == dragIndex_;
    const bool hovered = static_cast<int>(i) == top || carried;
    DrawWindow(i, viewport, fonts, hovered);
    if (carried || PointOnRim(rect, aim)) DrawRim(rect, viewport.Zoom(), carried);
  }
}

// ------------------------------------------------------------- selection

void ChatPanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport) {
  SelectCaught(windows_, lasso, [&](std::size_t i) { return ScreenRectOf(*windows_[i], viewport); });
}

void ChatPanel::Rects(const Viewport& viewport, std::vector<Rectangle>* out) const {
  AppendRects(windows_, out, [&](std::size_t i) { return ScreenRectOf(*windows_[i], viewport); });
}

void ChatPanel::ClearSelection() { ClearSelected(windows_); }

bool ChatPanel::AnySelected() const { return sb::AnySelected(windows_); }

bool ChatPanel::SelectedRimAt(Vector2 point, const Viewport& viewport) const {
  return SelectedRim(windows_, point,
                     [&](std::size_t i) { return ScreenRectOf(*windows_[i], viewport); });
}

void ChatPanel::BeginSelectionDrag() { RememberPositions(windows_); }

void ChatPanel::MoveSelection(Vector2 worldDelta) {
  MoveSelected(windows_, worldDelta);
  changed_ = changed_ || AnySelected();
}

void ChatPanel::SetSelectionAbsorbed(bool absorbed) {
  for (const std::unique_ptr<ChatWindow>& window : windows_) {
    if (window->selected) window->absorbed = absorbed;
  }
}

void ChatPanel::DeleteSelection() {
  for (std::size_t i = windows_.size(); i-- > 0;) {
    if (windows_[i]->selected) Remove(i);
  }
}

void ChatPanel::DrawSelectionRims(const Viewport& viewport) const {
  DrawSelectedRims(windows_, viewport.Zoom(),
                   [&](std::size_t i) { return ScreenRectOf(*windows_[i], viewport); });
}

}  // namespace sb
