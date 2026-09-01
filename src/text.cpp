#include "text.h"

namespace sb::text {

int NextCodepoint(std::string_view s, std::size_t* i) {
  if (*i >= s.size()) return 0;
  const unsigned char c = static_cast<unsigned char>(s[*i]);
  int cp = c;
  int extra = 0;
  if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
  else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
  else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
  ++(*i);
  for (int k = 0; k < extra && *i < s.size(); ++k) {
    cp = (cp << 6) | (static_cast<unsigned char>(s[*i]) & 0x3F);
    ++(*i);
  }
  return cp;
}

float Measure(FontStore& fonts, Face face, std::string_view utf8, float emPx) {
  float width = 0.0f;
  std::size_t i = 0;
  while (i < utf8.size()) {
    const int cp = NextCodepoint(utf8, &i);
    width += fonts.Advance(face, cp, emPx);
  }
  return width;
}

void Draw(FontStore& fonts, Face face, std::string_view utf8, Vector2 pen, float emPx,
          Color tint) {
  std::size_t i = 0;
  while (i < utf8.size()) {
    const int cp = NextCodepoint(utf8, &i);
    const Glyph g = fonts.GetGlyph(face, cp, emPx);
    if (!g.present) continue;
    if (g.atlas != nullptr && g.width > 0.0f && g.height > 0.0f) {
      const Rectangle dst = {pen.x + g.bearingX, pen.y - g.top, g.width, g.height};
      DrawTexturePro(*g.atlas, g.src, dst, {0.0f, 0.0f}, 0.0f, tint);
    }
    pen.x += g.advance;
  }
}

float Ascent(FontStore& fonts, Face face, float emPx) {
  return fonts.Metrics(face).ascent * emPx;
}

float Descent(FontStore& fonts, Face face, float emPx) {
  return fonts.Metrics(face).descent * emPx;
}

float LineHeight(FontStore& fonts, Face face, float emPx) {
  const FaceMetrics& m = fonts.Metrics(face);
  return (m.ascent + m.descent) * emPx * 1.12f;
}

std::vector<std::string> Wrap(FontStore& fonts, Face face, std::string_view utf8, float emPx,
                              float maxWidth) {
  std::vector<std::string> lines;
  if (utf8.empty()) return lines;

  std::string current;
  std::size_t i = 0;
  while (i < utf8.size()) {
    // Take the next word together with the whitespace that precedes it, so a
    // word only moves to the next line as a unit.
    std::size_t wordStart = i;
    while (i < utf8.size() && utf8[i] == ' ') ++i;
    while (i < utf8.size() && utf8[i] != ' ') ++i;
    std::string_view chunk = utf8.substr(wordStart, i - wordStart);

    if (current.empty()) {
      // Leading spaces at the start of a wrapped line just waste room.
      while (!chunk.empty() && chunk.front() == ' ') chunk.remove_prefix(1);
    }

    const std::string candidate = current + std::string(chunk);
    if (!current.empty() && Measure(fonts, face, candidate, emPx) > maxWidth) {
      lines.push_back(current);
      current.clear();
      while (!chunk.empty() && chunk.front() == ' ') chunk.remove_prefix(1);
      current = std::string(chunk);
    } else {
      current = candidate;
    }

    // A single word wider than the whole box still has to be broken somewhere.
    while (Measure(fonts, face, current, emPx) > maxWidth && current.size() > 1) {
      std::size_t cut = current.size();
      while (cut > 1 && Measure(fonts, face, std::string_view(current).substr(0, cut), emPx) >
                            maxWidth) {
        --cut;
        while (cut > 1 && (static_cast<unsigned char>(current[cut]) & 0xC0) == 0x80) --cut;
      }
      lines.push_back(current.substr(0, cut));
      current.erase(0, cut);
    }
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

}  // namespace sb::text
