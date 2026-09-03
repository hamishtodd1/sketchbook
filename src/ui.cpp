#include "ui.h"

#include <algorithm>
#include <cmath>

#include "text.h"
#include "theme.h"

namespace sb {
namespace {

constexpr float kPanelWidth = 420.0f;
constexpr float kPanelHeight = 420.0f;
constexpr float kTitleEm = 27.0f;
constexpr float kLabelEm = 19.0f;
constexpr float kHintEm = 15.0f;

// Both Update() and Draw() derive their geometry from here so the hit targets
// can never drift away from the things on screen.
struct MenuLayout {
  Rectangle panel;
  Rectangle row;
  Rectangle box;
  Rectangle aspect[kAspectCount];
  Rectangle save;
  Rectangle load;
  Rectangle insert;
};

MenuLayout Compute() {
  const float w = static_cast<float>(GetScreenWidth());
  const float h = static_cast<float>(GetScreenHeight());

  MenuLayout m;
  m.panel = {std::round((w - kPanelWidth) * 0.5f), std::round((h - kPanelHeight) * 0.5f),
             kPanelWidth, kPanelHeight};
  m.row = {m.panel.x + 20.0f, m.panel.y + 92.0f, m.panel.width - 40.0f, 46.0f};
  const float boxSide = 24.0f;
  m.box = {m.row.x + 12.0f, m.row.y + (m.row.height - boxSide) * 0.5f, boxSide, boxSide};

  // The slide shape, as the three it could sensibly be.
  const float aspectY = m.row.y + m.row.height + 34.0f;
  constexpr float kAspectGap = 8.0f;
  const float aspectWidth =
      (m.panel.width - 48.0f - kAspectGap * static_cast<float>(kAspectCount - 1)) / kAspectCount;
  for (int i = 0; i < kAspectCount; ++i) {
    m.aspect[i] = {m.panel.x + 24.0f + static_cast<float>(i) * (aspectWidth + kAspectGap),
                   aspectY, aspectWidth, 40.0f};
  }

  const float buttonWidth = (m.panel.width - 56.0f) * 0.5f;
  const float buttonY = aspectY + 40.0f + 30.0f;
  m.save = {m.panel.x + 24.0f, buttonY, buttonWidth, 44.0f};
  m.load = {m.save.x + buttonWidth + 8.0f, buttonY, buttonWidth, 44.0f};
  // Dropping a file on the window does this too, but there is nothing to drop
  // with on a tablet.
  m.insert = {m.panel.x + 24.0f, buttonY + 52.0f, m.panel.width - 48.0f, 44.0f};
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

}  // namespace

MenuAction OptionsMenu::Update(Options* options) {
  if (!visible_) return MenuAction::None;

  const MenuLayout m = Compute();
  const Vector2 mouse = GetMousePosition();
  if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return MenuAction::None;

  if (CheckCollisionPointRec(mouse, m.row)) {
    options->showGridDots = !options->showGridDots;
    return MenuAction::None;
  }
  for (int i = 0; i < kAspectCount; ++i) {
    if (CheckCollisionPointRec(mouse, m.aspect[i])) {
      options->slideAspect = kAspects[i];
      return MenuAction::None;
    }
  }
  if (CheckCollisionPointRec(mouse, m.save)) return MenuAction::Save;
  if (CheckCollisionPointRec(mouse, m.load)) return MenuAction::Load;
  if (CheckCollisionPointRec(mouse, m.insert)) return MenuAction::Insert;
  return MenuAction::None;
}

void OptionsMenu::Draw(FontStore& fonts, const Options& options) const {
  if (!visible_) return;

  const MenuLayout m = Compute();
  DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), kScrim);

  DrawRectangleRounded(m.panel, 0.06f, 12, kPanelBg);
  DrawRectangleRoundedLines(m.panel, 0.06f, 12, kPanelBorder);

  const float titleBaseline = m.panel.y + 26.0f + text::Ascent(fonts, Face::MainRegular, kTitleEm);
  text::Draw(fonts, Face::MainRegular, "Options", {m.panel.x + 24.0f, titleBaseline}, kTitleEm,
             kPanelText);

  DrawRectangle(static_cast<int>(m.panel.x + 24.0f), static_cast<int>(m.panel.y + 74.0f),
                static_cast<int>(m.panel.width - 48.0f), 1, kPanelBorder);

  // Toggle row.
  const Vector2 mouse = GetMousePosition();
  const bool hovered = CheckCollisionPointRec(mouse, m.row);
  if (hovered) DrawRectangleRounded(m.row, 0.25f, 8, Fade(kAccent, 0.08f));

  DrawRectangleRounded(m.box, 0.28f, 8,
                       options.showGridDots ? (hovered ? kAccentHover : kAccent) : kBoxIdle);
  DrawRectangleRoundedLines(m.box, 0.28f, 8, options.showGridDots ? kAccent : kBoxBorder);

  if (options.showGridDots) {
    // A tick, drawn rather than set in a font so it needs no glyph coverage.
    const Vector2 a = {m.box.x + m.box.width * 0.24f, m.box.y + m.box.height * 0.52f};
    const Vector2 b = {m.box.x + m.box.width * 0.43f, m.box.y + m.box.height * 0.72f};
    const Vector2 c = {m.box.x + m.box.width * 0.77f, m.box.y + m.box.height * 0.30f};
    DrawLineEx(a, b, 2.6f, RAYWHITE);
    DrawLineEx(b, c, 2.6f, RAYWHITE);
  }

  const float labelBaseline =
      m.row.y + (m.row.height - text::LineHeight(fonts, Face::MainRegular, kLabelEm)) * 0.5f +
      text::Ascent(fonts, Face::MainRegular, kLabelEm);
  text::Draw(fonts, Face::MainRegular, "Show grid dots",
             {m.box.x + m.box.width + 14.0f, labelBaseline}, kLabelEm, kPanelText);

  const float aspectLabelBaseline = m.aspect[0].y - 12.0f;
  text::Draw(fonts, Face::MainRegular, "Slide shape", {m.panel.x + 24.0f, aspectLabelBaseline},
             kHintEm, kPanelMuted);
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

  DrawButton(fonts, m.save, "Save canvas", CheckCollisionPointRec(mouse, m.save));
  DrawButton(fonts, m.load, "Load canvas", CheckCollisionPointRec(mouse, m.load));
  DrawButton(fonts, m.insert, "Insert picture or video",
             CheckCollisionPointRec(mouse, m.insert));

  const float hintBaseline = m.panel.y + m.panel.height - 22.0f;
  text::Draw(fonts, Face::MainRegular, "Esc to close", {m.panel.x + 24.0f, hintBaseline}, kHintEm,
             kPanelMuted);
}

}  // namespace sb
