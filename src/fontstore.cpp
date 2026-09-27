#include "fontstore.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sb {
namespace {

// --------------------------------------------------------------------------
// Just enough of the TrueType container to read head, hhea and cmap. raylib
// hands back rasterised bitmaps but not the font's own metrics, and we need
// those to make different faces agree on what "one em" means.
struct Reader {
  const unsigned char* d = nullptr;
  std::size_t          n = 0;

  std::uint8_t u8(std::size_t o) const { return (o < n) ? d[o] : 0; }
  std::uint16_t u16(std::size_t o) const {
    return (o + 2 <= n) ? static_cast<std::uint16_t>((d[o] << 8) | d[o + 1]) : 0;
  }
  std::int16_t i16(std::size_t o) const { return static_cast<std::int16_t>(u16(o)); }
  std::uint32_t u32(std::size_t o) const {
    if (o + 4 > n) return 0;
    return (static_cast<std::uint32_t>(d[o]) << 24) |
           (static_cast<std::uint32_t>(d[o + 1]) << 16) |
           (static_cast<std::uint32_t>(d[o + 2]) << 8) | static_cast<std::uint32_t>(d[o + 3]);
  }
};

constexpr std::uint32_t Tag(char a, char b, char c, char d) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

std::size_t FindTable(const Reader& r, std::uint32_t tag) {
  const std::uint16_t numTables = r.u16(4);
  for (std::uint16_t i = 0; i < numTables; ++i) {
    const std::size_t rec = 12 + 16u * i;
    if (r.u32(rec) == tag) return r.u32(rec + 8);
  }
  return 0;
}

void ParseCmapSubtable(const Reader& r, std::size_t sub, std::unordered_map<int, int>* out) {
  const std::uint16_t format = r.u16(sub);
  if (format == 4) {
    const std::uint16_t segX2 = r.u16(sub + 6);
    const std::uint16_t segs = segX2 / 2;
    const std::size_t endO = sub + 14;
    const std::size_t startO = endO + segX2 + 2;
    const std::size_t deltaO = startO + segX2;
    const std::size_t rangeO = deltaO + segX2;
    for (std::uint16_t s = 0; s < segs; ++s) {
      const std::uint16_t end = r.u16(endO + 2u * s);
      const std::uint16_t start = r.u16(startO + 2u * s);
      const std::int16_t  delta = r.i16(deltaO + 2u * s);
      const std::uint16_t rangeOff = r.u16(rangeO + 2u * s);
      if (start == 0xFFFF) continue;
      for (std::uint32_t c = start; c <= end; ++c) {
        std::uint16_t g = 0;
        if (rangeOff == 0) {
          g = static_cast<std::uint16_t>(c + delta);
        } else {
          const std::size_t gi = rangeO + 2u * s + rangeOff + 2u * (c - start);
          g = r.u16(gi);
          if (g != 0) g = static_cast<std::uint16_t>(g + delta);
        }
        if (g != 0) out->emplace(static_cast<int>(c), g);
        if (c == 0xFFFF) break;
      }
    }
  } else if (format == 12) {
    const std::uint32_t groups = r.u32(sub + 12);
    for (std::uint32_t i = 0; i < groups; ++i) {
      const std::size_t o = sub + 16 + 12u * i;
      const std::uint32_t start = r.u32(o);
      const std::uint32_t end = r.u32(o + 4);
      const std::uint32_t glyph = r.u32(o + 8);
      // Guard against a corrupt table claiming an absurd range.
      const std::uint32_t stop = std::min(end, start + 0x4000u);
      for (std::uint32_t c = start; c <= stop; ++c) {
        out->emplace(static_cast<int>(c), static_cast<int>(glyph + (c - start)));
      }
    }
  }
}

// A glyph's advance and outline box, in font units. False if the font has no
// glyf table to say (a CFF font), in which case the bitmaps are measured instead.
bool ReadOutline(const Reader& r, std::size_t head, std::size_t hhea, int glyph,
                 GlyphOutline* out) {
  const std::size_t hmtx = FindTable(r, Tag('h', 'm', 't', 'x'));
  const std::size_t loca = FindTable(r, Tag('l', 'o', 'c', 'a'));
  const std::size_t glyf = FindTable(r, Tag('g', 'l', 'y', 'f'));
  if (hmtx == 0 || loca == 0 || glyf == 0) return false;

  const int metrics = r.u16(hhea + 34);
  if (metrics <= 0) return false;
  out->advance = r.u16(hmtx + 4u * static_cast<std::size_t>(std::min(glyph, metrics - 1)));

  const bool longOffsets = r.i16(head + 50) != 0;
  const std::size_t g = static_cast<std::size_t>(glyph);
  const std::size_t from = longOffsets ? r.u32(loca + 4 * g) : 2u * r.u16(loca + 2 * g);
  const std::size_t to = longOffsets ? r.u32(loca + 4 * (g + 1)) : 2u * r.u16(loca + 2 * (g + 1));
  if (to > from) {   // an empty glyph -- a space -- has no outline and no box
    out->xMin = r.i16(glyf + from + 2);
    out->yMin = r.i16(glyf + from + 4);
    out->xMax = r.i16(glyf + from + 6);
    out->yMax = r.i16(glyf + from + 8);
  }
  return true;
}

// Reads the cmap plus head/hhea metrics, and every glyph's outline box. Returns
// false only if the file is not a TrueType container we understand.
bool ParseFontTables(const std::vector<unsigned char>& file, std::unordered_set<int>* coverage,
                     FaceMetrics* fm, std::unordered_map<int, GlyphOutline>* outlines) {
  if (file.size() < 12) return false;
  Reader r{file.data(), file.size()};

  const std::size_t head = FindTable(r, Tag('h', 'e', 'a', 'd'));
  const std::size_t hhea = FindTable(r, Tag('h', 'h', 'e', 'a'));
  const std::size_t cmap = FindTable(r, Tag('c', 'm', 'a', 'p'));
  if (head == 0 || hhea == 0 || cmap == 0) return false;

  fm->unitsPerEm = r.u16(head + 18);
  if (fm->unitsPerEm == 0) fm->unitsPerEm = 1000;
  fm->hheaAscent = r.i16(hhea + 4);
  fm->hheaDescent = r.i16(hhea + 6);
  if (fm->hheaAscent - fm->hheaDescent == 0) return false;
  fm->ascent = static_cast<float>(fm->hheaAscent) / static_cast<float>(fm->unitsPerEm);
  fm->descent = -static_cast<float>(fm->hheaDescent) / static_cast<float>(fm->unitsPerEm);

  // Prefer a full Unicode subtable, fall back to the BMP one.
  const std::uint16_t tables = r.u16(cmap + 2);
  std::size_t best = 0;
  int bestScore = -1;
  for (std::uint16_t i = 0; i < tables; ++i) {
    const std::size_t rec = cmap + 4 + 8u * i;
    const std::uint16_t plat = r.u16(rec);
    const std::uint16_t enc = r.u16(rec + 2);
    const std::size_t sub = cmap + r.u32(rec + 4);
    const std::uint16_t format = r.u16(sub);
    int score = -1;
    if (format == 12) score = 3;
    else if (format == 4 && plat == 3 && enc == 1) score = 2;
    else if (format == 4) score = 1;
    if (score > bestScore) { bestScore = score; best = sub; }
  }
  if (bestScore < 0) return false;
  std::unordered_map<int, int> glyphs;
  ParseCmapSubtable(r, best, &glyphs);
  bool haveOutlines = true;
  for (const auto& [codepoint, glyph] : glyphs) {
    coverage->insert(codepoint);
    GlyphOutline outline;
    if (haveOutlines && ReadOutline(r, head, hhea, glyph, &outline)) {
      outline.glyph = glyph;
      outlines->emplace(codepoint, outline);
    } else {
      haveOutlines = false;
    }
  }
  if (!haveOutlines) outlines->clear();
  return !coverage->empty();
}

// Atlases are only rasterised at these em sizes; anything in between is drawn
// from the nearest rung. Capped at 128px so a deep zoom cannot blow out VRAM.
constexpr float kLadder[] = {16.0f, 24.0f, 32.0f, 48.0f, 64.0f, 96.0f, 128.0f};
constexpr int   kLadderCount = static_cast<int>(sizeof(kLadder) / sizeof(kLadder[0]));

// Keep the cache bounded. Both limits must be exceeded before we evict, so a
// couple of large atlases in active use are never thrashed. Eleven faces, and a
// canvas can easily have three or four sizes of several of them on screen at
// once, so the entry count has to allow for that rather than for one box.
constexpr std::size_t kMaxTexels = 48ull * 1024 * 1024;
constexpr std::size_t kMaxEntries = 28;

int LadderRung(float emPx) {
  for (int i = 0; i < kLadderCount; ++i) {
    if (kLadder[i] >= emPx) return i;
  }
  return kLadderCount - 1;
}

const char* const kFaceFiles[kFaceCount] = {
    "KaTeX_Main-Regular.ttf",  "KaTeX_Main-Bold.ttf",     "KaTeX_Main-Italic.ttf",
    "KaTeX_Math-Italic.ttf",   "KaTeX_AMS-Regular.ttf",   "KaTeX_Caligraphic-Regular.ttf",
    "KaTeX_Size1-Regular.ttf", "KaTeX_Size2-Regular.ttf", "KaTeX_Size3-Regular.ttf",
    "KaTeX_Size4-Regular.ttf", "JetBrainsMono-Regular.ttf",
};

}  // namespace

FontStore::~FontStore() { Unload(); }

bool FontStore::Load(const std::string& fontDir, std::string* error) {
  for (int i = 0; i < kFaceCount; ++i) {
    const std::string path = fontDir + "/" + kFaceFiles[i];
    int size = 0;
    unsigned char* bytes = LoadFileData(path.c_str(), &size);
    if (bytes == nullptr || size <= 0) {
      if (error) *error = "could not read font: " + path;
      return false;
    }
    faces_[i].file.assign(bytes, bytes + size);
    UnloadFileData(bytes);

    if (!ParseFontTables(faces_[i].file, &faces_[i].coverage, &metrics_[i],
                         &faces_[i].outlines)) {
      if (error) *error = "unrecognised font container: " + path;
      return false;
    }

    // Rasterising every codepoint a font happens to carry is wasteful; JetBrains
    // Mono alone has over a thousand. Latin plus punctuation covers code, and
    // the KaTeX faces are small enough to take whole.
    const bool isMono = (static_cast<Face>(i) == Face::Mono);
    faces_[i].codepoints.reserve(faces_[i].coverage.size());
    for (int cp : faces_[i].coverage) {
      if (isMono && cp > 0x017F) continue;
      faces_[i].codepoints.push_back(cp);
    }
    std::sort(faces_[i].codepoints.begin(), faces_[i].codepoints.end());
    if (isMono) {
      // Drop anything we filtered out of the raster set from the coverage map
      // too, so HasGlyph never promises a glyph we did not bake.
      std::unordered_set<int> kept(faces_[i].codepoints.begin(), faces_[i].codepoints.end());
      faces_[i].coverage.swap(kept);
    }
    faces_[i].loaded = true;
  }
  return true;
}

void FontStore::Unload() {
  for (auto& entry : cache_) UnloadFont(entry.second.font);
  cache_.clear();
  strokes_.clear();
  texelsInUse_ = 0;
  for (int i = 0; i < kFaceCount; ++i) faces_[i] = FaceData{};
}

bool FontStore::HasGlyph(Face face, int codepoint) const {
  const FaceData& fd = faces_[static_cast<int>(face)];
  return fd.coverage.find(codepoint) != fd.coverage.end();
}

void FontStore::EvictIfNeeded() {
  while (cache_.size() > kMaxEntries || (texelsInUse_ > kMaxTexels && cache_.size() > 2)) {
    // Nothing used since the frame began may go. A glyph hands its caller a
    // pointer straight into the atlas texture, so unloading one that something
    // has already drawn from -- or is about to -- leaves that pointer dangling,
    // and the letters come out as smudges. It would also mean re-rasterising a
    // whole face every frame, which is not something to do inside a frame
    // budget. Over budget with nothing evictable, the cache simply runs large
    // for a frame; the bound applies again as soon as the screen changes.
    auto oldest = cache_.end();
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->second.lastUsed > frameMark_) continue;
      if (oldest == cache_.end() || it->second.lastUsed < oldest->second.lastUsed) oldest = it;
    }
    if (oldest == cache_.end()) break;
    texelsInUse_ -= oldest->second.texels;
    UnloadFont(oldest->second.font);
    cache_.erase(oldest);
  }
}

namespace {

// The atlas raylib would have built for us, built here instead.
//
// `LoadFontFromMemory` packs with method 0, which lays glyphs in rows of
// `fontSize` and copies each glyph's full height into one. That assumes no
// glyph is taller than the size it was asked for -- true of prose faces, and
// wildly untrue of KaTeX's Size1..Size4, where a big brace or a \bigwedge is
// several times the nominal size. Such a glyph is placed at an offset the
// bounds check calls legal and then written past the end of the bitmap, and
// `ImageFromImage` reads past it again a moment later. That is the crash: an
// access violation inside memcpy, under LoadFontFromMemory, reached from a
// box carrying an algebra whenever the zoom happened to land on a rung whose
// atlas came out half-height.
//
// So: method 1, which packs by each glyph's real rectangle and copies nothing
// it could not place. That alone is not quite enough -- raylib fills in a
// full-size rec for an unpacked glyph as well, and the read off the end would
// come back -- so anything that does not lie wholly inside the atlas is made
// empty before the atlas is cut up.
Font BuildFont(const std::vector<unsigned char>& file, const std::vector<int>& codepoints,
               int request) {
  constexpr int kPadding = 4;   // raylib's FONT_TTF_DEFAULT_CHARS_PADDING

  Font font{};
  font.baseSize = request;
  font.glyphCount = static_cast<int>(codepoints.size());
  font.glyphPadding = kPadding;
  font.glyphs = LoadFontData(file.data(), static_cast<int>(file.size()), request,
                             const_cast<int*>(codepoints.data()), font.glyphCount, FONT_DEFAULT);
  if (font.glyphs == nullptr) {
    font.glyphCount = 0;
    return font;
  }

  Image atlas = GenImageFontAtlas(font.glyphs, &font.recs, font.glyphCount, request, kPadding,
                                  /*packMethod=*/1);
  if (atlas.data == nullptr) {
    UnloadFontData(font.glyphs, font.glyphCount);
    font.glyphs = nullptr;
    font.glyphCount = 0;
    return font;
  }

  // A glyph that did not fit is nothing rather than a read off the end. Checked
  // against the bitmap rather than against the size that was asked for, which
  // is the assumption that was wrong in the first place.
  for (int i = 0; i < font.glyphCount; ++i) {
    Rectangle& r = font.recs[i];
    if (r.x < 0.0f || r.y < 0.0f || r.width <= 0.0f || r.height <= 0.0f ||
        r.x + r.width > static_cast<float>(atlas.width) ||
        r.y + r.height > static_cast<float>(atlas.height)) {
      r = {0.0f, 0.0f, 0.0f, 0.0f};
    }
  }

  font.texture = LoadTextureFromImage(atlas);
  for (int i = 0; i < font.glyphCount; ++i) {
    UnloadImage(font.glyphs[i].image);
    font.glyphs[i].image = ImageFromImage(atlas, font.recs[i]);
  }
  UnloadImage(atlas);
  return font;
}

}  // namespace

FontStore::Sized* FontStore::Acquire(Face face, float emPx) {
  const int faceIdx = static_cast<int>(face);
  FaceData& fd = faces_[faceIdx];
  if (!fd.loaded) return nullptr;

  const int rung = LadderRung(emPx);
  const std::uint64_t key =
      (static_cast<std::uint64_t>(faceIdx) << 32) | static_cast<std::uint32_t>(rung);

  auto found = cache_.find(key);
  if (found != cache_.end()) {
    found->second.lastUsed = ++clock_;
    return &found->second;
  }

  // raylib's fontSize argument is a stbtt pixel *height* (ascent to descent),
  // not an em. Faces disagree on that ratio, so ask each for the size that
  // actually yields the em we want -- otherwise mixing Main and Math-Italic at
  // "the same size" silently sets them at different scales.
  const FaceMetrics& fm = metrics_[faceIdx];
  const float span = static_cast<float>(fm.hheaAscent - fm.hheaDescent);
  const float em = kLadder[rung];
  int request = static_cast<int>(std::lround(em * span / static_cast<float>(fm.unitsPerEm)));
  if (request < 1) request = 1;

  Sized sized;
  // The em the atlas was actually rasterised at. `request` is a whole number of
  // pixels, so it is not quite `em`, and scaling the bitmaps by the rung's
  // nominal size would draw every glyph a percent or two off its outline.
  sized.emPx = static_cast<float>(request) * static_cast<float>(fm.unitsPerEm) / span;
  sized.font = BuildFont(fd.file, fd.codepoints, request);
  if (sized.font.glyphCount <= 0 || sized.font.texture.id == 0) {
    UnloadFont(sized.font);
    return nullptr;
  }
  SetTextureFilter(sized.font.texture, TEXTURE_FILTER_BILINEAR);

  // Mirrors raylib's own offsetY += (int)(ascent*scaleFactor) so we can undo it
  // and recover a true baseline.
  const float scaleFactor = static_cast<float>(request) / span;
  sized.baseline =
      static_cast<float>(static_cast<int>(static_cast<float>(fm.hheaAscent) * scaleFactor));

  sized.index.reserve(static_cast<std::size_t>(sized.font.glyphCount) * 2);
  for (int i = 0; i < sized.font.glyphCount; ++i) {
    sized.index.emplace(sized.font.glyphs[i].value, i);
  }
  sized.texels = static_cast<std::size_t>(sized.font.texture.width) *
                 static_cast<std::size_t>(sized.font.texture.height);
  sized.lastUsed = ++clock_;

  texelsInUse_ += sized.texels;
  cache_.emplace(key, std::move(sized));
  EvictIfNeeded();

  // Eviction may have dropped the entry we just added if it alone blows the
  // budget, so look it up again rather than trusting a stale iterator.
  auto again = cache_.find(key);
  return (again == cache_.end()) ? nullptr : &again->second;
}

Glyph FontStore::GetGlyph(Face face, int codepoint, float emPx) {
  Glyph g;
  if (!HasGlyph(face, codepoint)) return g;

  Sized* sized = Acquire(face, emPx);
  if (sized == nullptr) return g;

  auto it = sized->index.find(codepoint);
  if (it == sized->index.end()) return g;

  const int slot = it->second;
  const GlyphInfo& gi = sized->font.glyphs[slot];
  const Rectangle& rec = sized->font.recs[slot];

  // raylib leaves advanceX at 0 for a codepoint the face could not render.
  if (gi.advanceX == 0 && rec.width <= 0.0f && codepoint != ' ') return g;

  const float s = emPx / sized->emPx;
  g.present = true;
  g.drawLeft = static_cast<float>(gi.offsetX) * s;
  g.drawTop = (sized->baseline - static_cast<float>(gi.offsetY)) * s;
  g.drawWidth = rec.width * s;
  g.drawHeight = rec.height * s;
  g.src = rec;
  g.atlas = &sized->font.texture;

  const FaceData& fd = faces_[static_cast<int>(face)];
  const auto outline = fd.outlines.find(codepoint);
  if (outline == fd.outlines.end()) {
    // No outline to go by: measure the bitmap, as everything used to.
    g.advance = static_cast<float>(gi.advanceX) * s;
    g.bearingX = g.drawLeft;
    g.top = g.drawTop;
    g.width = g.drawWidth;
    g.height = g.drawHeight;
    return g;
  }
  const float unit = emPx / static_cast<float>(metrics_[static_cast<int>(face)].unitsPerEm);
  const GlyphOutline& o = outline->second;
  g.advance = static_cast<float>(o.advance) * unit;
  g.bearingX = static_cast<float>(o.xMin) * unit;
  g.top = static_cast<float>(o.yMax) * unit;
  g.width = static_cast<float>(o.xMax - o.xMin) * unit;
  g.height = static_cast<float>(o.yMax - o.yMin) * unit;
  return g;
}

namespace {

struct Point {
  float x = 0.0f;
  float y = 0.0f;
};

// A simple glyph's contours, with its quadratic curves flattened into short
// straight pieces. Empty for a composite glyph, which a one-stroke accent never
// is.
std::vector<std::vector<Point>> Contours(const std::vector<unsigned char>& file, int glyph) {
  std::vector<std::vector<Point>> out;
  Reader r{file.data(), file.size()};
  const std::size_t head = FindTable(r, Tag('h', 'e', 'a', 'd'));
  const std::size_t loca = FindTable(r, Tag('l', 'o', 'c', 'a'));
  const std::size_t glyf = FindTable(r, Tag('g', 'l', 'y', 'f'));
  if (head == 0 || loca == 0 || glyf == 0) return out;

  const bool        longOffsets = r.i16(head + 50) != 0;
  const std::size_t g = static_cast<std::size_t>(glyph);
  const std::size_t from = longOffsets ? r.u32(loca + 4 * g) : 2u * r.u16(loca + 2 * g);
  const std::size_t to = longOffsets ? r.u32(loca + 4 * (g + 1)) : 2u * r.u16(loca + 2 * (g + 1));
  if (to <= from) return out;
  std::size_t p = glyf + from;
  const int contours = r.i16(p);
  if (contours <= 0) return out;
  p += 10;

  std::vector<int> ends(static_cast<std::size_t>(contours));
  for (int& end : ends) { end = r.u16(p); p += 2; }
  const int count = ends.back() + 1;
  if (count <= 0 || count > 4096) return out;
  p += 2 + r.u16(p);   // the hinting instructions, which are not wanted

  std::vector<std::uint8_t> flags(static_cast<std::size_t>(count));
  for (int i = 0; i < count;) {
    const std::uint8_t f = r.u8(p++);
    flags[static_cast<std::size_t>(i++)] = f;
    if ((f & 0x08) != 0) {
      for (int repeat = r.u8(p++); repeat > 0 && i < count; --repeat) {
        flags[static_cast<std::size_t>(i++)] = f;
      }
    }
  }
  std::vector<Point> points(static_cast<std::size_t>(count));
  int x = 0;
  for (int i = 0; i < count; ++i) {
    const std::uint8_t f = flags[static_cast<std::size_t>(i)];
    if ((f & 0x02) != 0) {
      const int dx = r.u8(p++);
      x += ((f & 0x10) != 0) ? dx : -dx;
    } else if ((f & 0x10) == 0) {
      x += r.i16(p);
      p += 2;
    }
    points[static_cast<std::size_t>(i)].x = static_cast<float>(x);
  }
  int y = 0;
  for (int i = 0; i < count; ++i) {
    const std::uint8_t f = flags[static_cast<std::size_t>(i)];
    if ((f & 0x04) != 0) {
      const int dy = r.u8(p++);
      y += ((f & 0x20) != 0) ? dy : -dy;
    } else if ((f & 0x20) == 0) {
      y += r.i16(p);
      p += 2;
    }
    points[static_cast<std::size_t>(i)].y = static_cast<float>(y);
  }

  // Flattening. Two off-curve points in a row have an on-curve one implied
  // halfway between them, which is how TrueType spells a run of curves.
  int start = 0;
  for (int end : ends) {
    const int m = end - start + 1;
    if (m < 3) { start = end + 1; continue; }
    const auto at = [&](int k) -> const Point& {
      return points[static_cast<std::size_t>(start + ((k % m) + m) % m)];
    };
    const auto on = [&](int k) {
      return (flags[static_cast<std::size_t>(start + ((k % m) + m) % m)] & 0x01) != 0;
    };
    int first = 0;
    while (first < m && !on(first)) ++first;
    Point cur = (first < m) ? at(first)
                            : Point{(at(0).x + at(1).x) * 0.5f, (at(0).y + at(1).y) * 0.5f};
    if (first >= m) first = 0;

    std::vector<Point> poly{cur};
    bool  pending = false;
    Point control{};
    const auto curve = [&poly](Point a, Point b, Point c) {
      constexpr int kSteps = 8;
      for (int s = 1; s <= kSteps; ++s) {
        const float t = static_cast<float>(s) / kSteps;
        const float u = 1.0f - t;
        poly.push_back({u * u * a.x + 2.0f * u * t * b.x + t * t * c.x,
                        u * u * a.y + 2.0f * u * t * b.y + t * t * c.y});
      }
    };
    for (int k = 1; k <= m; ++k) {
      const Point next = at(first + k);
      if (on(first + k)) {
        if (pending) curve(cur, control, next); else poly.push_back(next);
        cur = next;
        pending = false;
      } else if (pending) {
        const Point mid{(control.x + next.x) * 0.5f, (control.y + next.y) * 0.5f};
        curve(cur, control, mid);
        cur = mid;
        control = next;
      } else {
        control = next;
        pending = true;
      }
    }
    if (pending) curve(cur, control, at(first));
    out.push_back(std::move(poly));
    start = end + 1;
  }
  return out;
}

}  // namespace

const StrokeProfile* FontStore::Stroke(Face face, int codepoint) {
  const std::uint64_t key =
      (static_cast<std::uint64_t>(static_cast<int>(face)) << 32) | static_cast<std::uint32_t>(codepoint);
  if (auto found = strokes_.find(key); found != strokes_.end()) {
    return found->second.centre.empty() ? nullptr : &found->second;
  }
  StrokeProfile& profile = strokes_[key];   // an empty one records "no stroke here"

  const FaceData& fd = faces_[static_cast<int>(face)];
  const auto outline = fd.outlines.find(codepoint);
  if (!fd.loaded || outline == fd.outlines.end()) return nullptr;
  const GlyphOutline& o = outline->second;
  if (o.xMax <= o.xMin) return nullptr;
  const std::vector<std::vector<Point>> contours = Contours(fd.file, o.glyph);
  if (contours.empty()) return nullptr;

  // A line straight up through the stroke crosses it twice: the highest and the
  // lowest crossing are its two edges there. Sampled just inside the ends, where
  // there is still a stroke to cross.
  constexpr int kSamples = 97;
  const float unit = 1.0f / static_cast<float>(metrics_[static_cast<int>(face)].unitsPerEm);
  const float left = static_cast<float>(o.xMin);
  const float span = static_cast<float>(o.xMax - o.xMin);
  float lastCentre = 0.0f;
  float lastThickness = 0.0f;
  for (int i = 0; i < kSamples; ++i) {
    const float x = left + span * (0.001f + 0.998f * static_cast<float>(i) / (kSamples - 1));
    float top = -1e9f;
    float bottom = 1e9f;
    for (const std::vector<Point>& poly : contours) {
      for (std::size_t k = 0; k < poly.size(); ++k) {
        const Point& a = poly[k];
        const Point& b = poly[(k + 1) % poly.size()];
        if ((a.x - x) * (b.x - x) > 0.0f || a.x == b.x) continue;
        const float y = a.y + (x - a.x) / (b.x - a.x) * (b.y - a.y);
        top = std::max(top, y);
        bottom = std::min(bottom, y);
      }
    }
    if (top > bottom) {
      lastCentre = (top + bottom) * 0.5f * unit;
      lastThickness = (top - bottom) * unit;
    }
    profile.centre.push_back(lastCentre);
    profile.thickness.push_back(lastThickness);
  }
  profile.width = span * unit;
  return &profile;
}

float FontStore::Advance(Face face, int codepoint, float emPx) {
  // Straight from the font's tables where it can be, which is exact at every
  // size and needs no atlas.
  const FaceData& fd = faces_[static_cast<int>(face)];
  if (HasGlyph(face, codepoint)) {
    const auto outline = fd.outlines.find(codepoint);
    if (outline != fd.outlines.end()) {
      return static_cast<float>(outline->second.advance) * emPx /
             static_cast<float>(metrics_[static_cast<int>(face)].unitsPerEm);
    }
  }
  const Glyph g = GetGlyph(face, codepoint, emPx);
  return g.present ? g.advance : 0.0f;
}

}  // namespace sb
