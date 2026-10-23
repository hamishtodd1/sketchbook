#include "ui.h"
#include "screen.h"

#include <algorithm>
#include <cmath>

#include "recent.h"
#include "text.h"
#include "theme.h"

namespace sb {
namespace {

constexpr float kPanelWidth = 420.0f;
constexpr float kLabelEm = 19.0f;
constexpr float kHintEm = 15.0f;

constexpr float kRowHeight = 40.0f;
constexpr float kRecentHeight = 32.0f;
constexpr float kButtonHeight = 44.0f;

// Both Update() and Draw() derive their geometry from here so the hit targets
// can never drift away from the things on screen.
struct MenuLayout {
  Rectangle panel;
  Rectangle aspectHeader;
  Rectangle aspect[kAspectCount];
  bool      aspectOpen = false;
  float     recentLabelTop = 0.0f;
  Rectangle recent[recent::kMaxRecent];
  int       recentRows = 0;
  Rectangle fresh;
  Rectangle save;
  Rectangle load;
  Rectangle insert;
  Rectangle slide;
  Rectangle ask;
  Rectangle record;
  Rectangle alwaysCode;
  Rectangle apiKey;
  Rectangle fullscreen;
};

// The panel is as tall as what is in it, since what is in it changes: the slide
// shapes fold away and the recent list is however long it is.
MenuLayout Compute(bool aspectOpen, int recentCount) {
  MenuLayout m;
  m.aspectOpen = aspectOpen;
  m.recentRows = std::clamp(recentCount, 0, recent::kMaxRecent);

  const float inset = 24.0f;
  const float inner = kPanelWidth - inset * 2.0f;

  // Laid out relative to the panel's top, so the height can be totalled before
  // anywhere on screen has been decided. No title: everybody who has pressed
  // Escape knows which menu this is.
  float y = inset;
  m.aspectHeader = {inset, y, inner, kRowHeight};
  y += kRowHeight;
  if (aspectOpen) {
    y += 10.0f;
    constexpr float kAspectGap = 8.0f;
    const float aspectWidth =
        (inner - kAspectGap * static_cast<float>(kAspectCount - 1)) / kAspectCount;
    for (int i = 0; i < kAspectCount; ++i) {
      m.aspect[i] = {inset + static_cast<float>(i) * (aspectWidth + kAspectGap), y, aspectWidth,
                     kRowHeight};
    }
    y += kRowHeight;
  }
  y += 22.0f;

  if (m.recentRows > 0) {
    m.recentLabelTop = y;
    y += 22.0f;
    for (int i = 0; i < m.recentRows; ++i) {
      m.recent[i] = {inset, y, inner, kRecentHeight};
      y += kRecentHeight + 2.0f;
    }
    y += 20.0f;
  }

  // A blank canvas. The app opens on the last one, so this is the way to a new
  // one -- and the one being left is already saved.
  m.fresh = {inset, y, inner, kButtonHeight};
  y += kButtonHeight + 8.0f;
  const float buttonWidth = (inner - 8.0f) * 0.5f;
  m.save = {inset, y, buttonWidth, kButtonHeight};
  m.load = {inset + buttonWidth + 8.0f, y, buttonWidth, kButtonHeight};
  y += kButtonHeight + 8.0f;
  // Dropping a file on the window does this too, but there is nothing to drop
  // with on a tablet.
  m.insert = {inset, y, inner, kButtonHeight};
  y += kButtonHeight + 8.0f;
  // Slides come from here, and only from here.
  m.slide = {inset, y, inner, kButtonHeight};
  y += kButtonHeight + 8.0f;
  // A conversation with Claude, in the middle of the view; and recording the
  // session, for a video made from it later (`--edit`).
  m.ask = {inset, y, buttonWidth, kButtonHeight};
  m.record = {inset + buttonWidth + 8.0f, y, buttonWidth, kButtonHeight};
  y += kButtonHeight + 8.0f;
  // How boxes read. A tickbox rather than a button, since it is a state that
  // stays put while the menu is up, and the tick is what says which it is in.
  m.alwaysCode = {inset, y, inner, kRowHeight};
  y += kRowHeight + 8.0f;
  // The Claude API key, pasted rather than typed: it is copied from the
  // Console, and a key never has to be shown on screen to be given.
  m.apiKey = {inset, y, inner, kButtonHeight};
  y += kButtonHeight + 8.0f;
  // The whole screen, title bar and all. It is a thing done to the window
  // rather than to the canvas, so it comes last.
  m.fullscreen = {inset, y, inner, kButtonHeight};
  y += kButtonHeight;
  y += inset;   // the panel's own bottom margin, the same as its top

  const float w = static_cast<float>(ScreenWidth());
  const float h = static_cast<float>(ScreenHeight());
  m.panel = {std::round((w - kPanelWidth) * 0.5f), std::round((h - y) * 0.5f), kPanelWidth, y};

  // Everything above was relative; move it onto the panel in one pass, so there
  // is one place the offset is applied and nothing can be missed.
  const Vector2 at = {m.panel.x, m.panel.y};
  auto place = [at](Rectangle* r) {
    r->x += at.x;
    r->y += at.y;
  };
  place(&m.aspectHeader);
  for (int i = 0; i < kAspectCount; ++i) place(&m.aspect[i]);
  for (int i = 0; i < m.recentRows; ++i) place(&m.recent[i]);
  place(&m.fresh);
  place(&m.save);
  place(&m.load);
  place(&m.insert);
  place(&m.slide);
  place(&m.ask);
  place(&m.record);
  place(&m.alwaysCode);
  place(&m.apiKey);
  place(&m.fullscreen);
  m.recentLabelTop += at.y;
  return m;
}

void DrawButton(FontStore& fonts, Rectangle rect, const char* label, bool hovered) {
  DrawRectangleRounded(rect, 0.22f, 10, hovered ? kAccentHover : kAccent);
  const float width = text::Measure(fonts, Face::MainRegular, label, kLabelEm);
  const float baseline =
      rect.y + rect.height * 0.5f + text::Ascent(fonts, Face::MainRegular, kLabelEm) * 0.5f -
      text::Descent(fonts, Face::MainRegular, kLabelEm) * 0.25f;
  text::Draw(fonts, Face::MainRegular, label, {rect.x + (rect.width - width) * 0.5f, baseline},
             kLabelEm, RAYWHITE);
}

// A chevron, drawn rather than set in a font so it needs no glyph coverage --
// the same reasoning as the tick that used to sit beside the grid toggle. It
// points at what it would do: right to open, down when already open.
void DrawChevron(Vector2 centre, bool open, Color tint) {
  constexpr float kArm = 5.0f;
  if (open) {
    DrawLineEx({centre.x - kArm, centre.y - kArm * 0.5f}, {centre.x, centre.y + kArm * 0.5f}, 2.4f,
               tint);
    DrawLineEx({centre.x, centre.y + kArm * 0.5f}, {centre.x + kArm, centre.y - kArm * 0.5f}, 2.4f,
               tint);
  } else {
    DrawLineEx({centre.x - kArm * 0.5f, centre.y - kArm}, {centre.x + kArm * 0.5f, centre.y}, 2.4f,
               tint);
    DrawLineEx({centre.x + kArm * 0.5f, centre.y}, {centre.x - kArm * 0.5f, centre.y + kArm}, 2.4f,
               tint);
  }
}

// A tickbox, the tick drawn rather than set in a font for the same reason as
// the chevron.
void DrawTickbox(FontStore& fonts, Rectangle row, const char* label, bool ticked, bool hovered) {
  if (hovered) DrawRectangleRounded(row, 0.25f, 8, Fade(kAccent, 0.08f));
  constexpr float kSide = 20.0f;
  const Rectangle box = {row.x + 8.0f, row.y + (row.height - kSide) * 0.5f, kSide, kSide};
  DrawRectangleRounded(box, 0.25f, 6, ticked ? kAccent : kBoxIdle);
  DrawRectangleRoundedLines(box, 0.25f, 6, ticked ? kAccent : kBoxBorder);
  if (ticked) {
    const Vector2 a = {box.x + kSide * 0.24f, box.y + kSide * 0.52f};
    const Vector2 b = {box.x + kSide * 0.43f, box.y + kSide * 0.72f};
    const Vector2 c = {box.x + kSide * 0.78f, box.y + kSide * 0.30f};
    DrawLineEx(a, b, 2.6f, RAYWHITE);
    DrawLineEx(b, c, 2.6f, RAYWHITE);
  }
  const float baseline = row.y + (row.height - text::LineHeight(fonts, Face::MainRegular, kLabelEm)) *
                                     0.5f +
                         text::Ascent(fonts, Face::MainRegular, kLabelEm);
  text::Draw(fonts, Face::MainRegular, label, {box.x + kSide + 12.0f, baseline}, kLabelEm,
             kPanelText);
}

const char* AspectName(float aspect) {
  for (int i = 0; i < kAspectCount; ++i) {
    if (std::abs(aspect - kAspects[i]) < 0.001f) return kAspectNames[i];
  }
  return "custom";
}

// What to call a canvas in the list. The name is what anyone recognises; the
// folder is only there to tell two files of the same name apart.
std::string FileName(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  return (cut == std::string::npos) ? path : path.substr(cut + 1);
}

std::string FolderName(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  return (cut == std::string::npos) ? std::string() : path.substr(0, cut);
}

float TextBaseline(FontStore& fonts, Rectangle rect, float em) {
  return rect.y + (rect.height - text::LineHeight(fonts, Face::MainRegular, em)) * 0.5f +
         text::Ascent(fonts, Face::MainRegular, em);
}

}  // namespace

void OptionsMenu::Toggle() {
  visible_ = !visible_;
  if (visible_) recent_ = recent::List();
  chosen_.clear();
}

MenuAction OptionsMenu::Update(Options* options) {
  if (!visible_) return MenuAction::None;

  const MenuLayout m = Compute(aspectOpen_, static_cast<int>(recent_.size()));
  const Vector2 mouse = GetMousePosition();
  if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return MenuAction::None;

  // A press anywhere off the panel puts the menu away. The press is still the
  // menu's -- it does not go on to land on whatever is underneath.
  if (!CheckCollisionPointRec(mouse, m.panel)) {
    Close();
    return MenuAction::None;
  }

  if (CheckCollisionPointRec(mouse, m.aspectHeader)) {
    aspectOpen_ = !aspectOpen_;
    return MenuAction::None;
  }
  if (aspectOpen_) {
    for (int i = 0; i < kAspectCount; ++i) {
      if (CheckCollisionPointRec(mouse, m.aspect[i])) {
        options->slideAspect = kAspects[i];
        return MenuAction::None;
      }
    }
  }
  for (int i = 0; i < m.recentRows; ++i) {
    if (CheckCollisionPointRec(mouse, m.recent[i])) {
      chosen_ = recent_[static_cast<std::size_t>(i)];
      return MenuAction::OpenRecent;
    }
  }
  if (CheckCollisionPointRec(mouse, m.fresh)) return MenuAction::NewCanvas;
  if (CheckCollisionPointRec(mouse, m.save)) return MenuAction::Save;
  if (CheckCollisionPointRec(mouse, m.load)) return MenuAction::Load;
  if (CheckCollisionPointRec(mouse, m.insert)) return MenuAction::Insert;
  if (CheckCollisionPointRec(mouse, m.slide)) return MenuAction::NewSlide;
  if (CheckCollisionPointRec(mouse, m.ask)) return MenuAction::AskClaude;
  if (CheckCollisionPointRec(mouse, m.record)) return MenuAction::Record;
  if (CheckCollisionPointRec(mouse, m.alwaysCode)) return MenuAction::AlwaysCode;
  if (CheckCollisionPointRec(mouse, m.apiKey)) return MenuAction::PasteApiKey;
  if (CheckCollisionPointRec(mouse, m.fullscreen)) return MenuAction::Fullscreen;
  return MenuAction::None;
}

void OptionsMenu::Draw(FontStore& fonts, const Options& options) const {
  if (!visible_) return;

  const MenuLayout m = Compute(aspectOpen_, static_cast<int>(recent_.size()));
  DrawRectangle(0, 0, ScreenWidth(), ScreenHeight(), kScrim);

  DrawRectangleRounded(m.panel, 0.06f, 12, kPanelBg);
  DrawRectangleRoundedLines(m.panel, 0.06f, 12, kPanelBorder);

  const Vector2 mouse = GetMousePosition();

  // --- the slide shape, folded away until it is asked for --------------------
  const bool overHeader = CheckCollisionPointRec(mouse, m.aspectHeader);
  if (overHeader) DrawRectangleRounded(m.aspectHeader, 0.25f, 8, Fade(kAccent, 0.08f));

  const float headerBaseline = TextBaseline(fonts, m.aspectHeader, kLabelEm);
  text::Draw(fonts, Face::MainRegular, "Slide shape",
             {m.aspectHeader.x + 26.0f, headerBaseline}, kLabelEm, kPanelText);
  DrawChevron({m.aspectHeader.x + 11.0f, m.aspectHeader.y + m.aspectHeader.height * 0.5f},
              aspectOpen_, overHeader ? kPanelText : kPanelMuted);

  // The chosen shape is written on the header, so folding the four away does
  // not fold away which one it is.
  if (!aspectOpen_) {
    const char* current = AspectName(options.slideAspect);
    const float width = text::Measure(fonts, Face::MainRegular, current, kLabelEm);
    text::Draw(fonts, Face::MainRegular, current,
               {m.aspectHeader.x + m.aspectHeader.width - width - 12.0f, headerBaseline}, kLabelEm,
               kPanelMuted);
  } else {
    for (int i = 0; i < kAspectCount; ++i) {
      const bool chosen = std::abs(options.slideAspect - kAspects[i]) < 0.001f;
      const bool over = CheckCollisionPointRec(mouse, m.aspect[i]);
      DrawRectangleRounded(m.aspect[i], 0.25f, 8,
                           chosen ? (over ? kAccentHover : kAccent) : kBoxIdle);
      DrawRectangleRoundedLines(m.aspect[i], 0.25f, 8, chosen ? kAccent : kBoxBorder);
      const float w = text::Measure(fonts, Face::MainRegular, kAspectNames[i], kLabelEm);
      text::Draw(fonts, Face::MainRegular, kAspectNames[i],
                 {m.aspect[i].x + (m.aspect[i].width - w) * 0.5f,
                  m.aspect[i].y + m.aspect[i].height * 0.5f + kLabelEm * 0.34f},
                 kLabelEm, chosen ? RAYWHITE : kPanelText);
    }
  }

  // --- the canvases opened lately -------------------------------------------
  if (m.recentRows > 0) {
    text::Draw(fonts, Face::MainRegular, "Recent",
               {m.panel.x + 24.0f, m.recentLabelTop + text::Ascent(fonts, Face::MainRegular,
                                                                   kHintEm)},
               kHintEm, kPanelMuted);
  }
  for (int i = 0; i < m.recentRows; ++i) {
    const std::string& path = recent_[static_cast<std::size_t>(i)];
    const bool over = CheckCollisionPointRec(mouse, m.recent[i]);
    if (over) DrawRectangleRounded(m.recent[i], 0.25f, 8, Fade(kAccent, 0.12f));

    const std::string name = FileName(path);
    const float baseline = TextBaseline(fonts, m.recent[i], kLabelEm);
    const float nameWidth =
        text::Measure(fonts, Face::MainRegular, name.c_str(), kLabelEm);
    text::Draw(fonts, Face::MainRegular, name.c_str(), {m.recent[i].x + 12.0f, baseline}, kLabelEm,
               kPanelText);

    // The folder only earns its place when there is room for it: it is there to
    // tell two files of the same name apart, not to be read.
    const std::string folder = FolderName(path);
    if (folder.empty()) continue;
    const float folderWidth =
        text::Measure(fonts, Face::MainRegular, folder.c_str(), kHintEm);
    const float room = m.recent[i].width - 24.0f - nameWidth - 16.0f;
    if (folderWidth > room) continue;
    text::Draw(fonts, Face::MainRegular, folder.c_str(),
               {m.recent[i].x + m.recent[i].width - folderWidth - 12.0f, baseline}, kHintEm,
               kPanelMuted);
  }

  DrawButton(fonts, m.fresh, "New canvas", CheckCollisionPointRec(mouse, m.fresh));
  DrawButton(fonts, m.save, "Save canvas", CheckCollisionPointRec(mouse, m.save));
  DrawButton(fonts, m.load, "Load canvas", CheckCollisionPointRec(mouse, m.load));
  DrawButton(fonts, m.insert, "Insert picture or video",
             CheckCollisionPointRec(mouse, m.insert));
  DrawButton(fonts, m.slide, "New slide", CheckCollisionPointRec(mouse, m.slide));
  DrawButton(fonts, m.ask, "Ask Claude", CheckCollisionPointRec(mouse, m.ask));
  DrawButton(fonts, m.record, options.recording ? "Stop recording" : "Record",
             CheckCollisionPointRec(mouse, m.record));
  DrawTickbox(fonts, m.alwaysCode, "Always show code", options.alwaysCode,
              CheckCollisionPointRec(mouse, m.alwaysCode));
  // Says what pressing it does, and which key is set if one is.
  const std::string keyLabel =
      !options.apiKeyNote.empty() ? options.apiKeyNote
      : options.apiKeyTail.empty() ? std::string("Paste Claude API key")
                                   : "API key ..." + options.apiKeyTail + " (paste to replace)";
  DrawButton(fonts, m.apiKey, keyLabel.c_str(), CheckCollisionPointRec(mouse, m.apiKey));
  // Says what pressing it would give you, like every other toggle here.
  DrawButton(fonts, m.fullscreen,
             options.fullscreen ? "Leave fullscreen" : "Fullscreen",
             CheckCollisionPointRec(mouse, m.fullscreen));
}

}  // namespace sb
