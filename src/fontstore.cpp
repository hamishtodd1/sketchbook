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

void ParseCmapSubtable(const Reader& r, std::size_t sub, std::unordered_set<int>* out) {
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
        if (g != 0) out->insert(static_cast<int>(c));
        if (c == 0xFFFF) break;
      }
    }
  } else if (format == 12) {
    const std::uint32_t groups = r.u32(sub + 12);
    for (std::uint32_t i = 0; i < groups; ++i) {
      const std::size_t o = sub + 16 + 12u * i;
      const std::uint32_t start = r.u32(o);
      const std::uint32_t end = r.u32(o + 4);
      // Guard against a corrupt table claiming an absurd range.
      const std::uint32_t stop = std::min(end, start + 0x4000u);
      for (std::uint32_t c = start; c <= stop; ++c) out->insert(static_cast<int>(c));
    }
  }
}

// Reads the cmap plus head/hhea metrics. Returns false only if the file is not
// a TrueType container we understand.
bool ParseFontTables(const std::vector<unsigned char>& file, std::unordered_set<int>* coverage,
                     FaceMetrics* fm) {
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
  ParseCmapSubtable(r, best, coverage);
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

    if (!ParseFontTables(faces_[i].file, &faces_[i].coverage, &metrics_[i])) {
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
  sized.emPx = em;
  sized.font = LoadFontFromMemory(".ttf", fd.file.data(), static_cast<int>(fd.file.size()), request,
                                  fd.codepoints.data(), static_cast<int>(fd.codepoints.size()));
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
  g.advance = static_cast<float>(gi.advanceX) * s;
  g.bearingX = static_cast<float>(gi.offsetX) * s;
  g.top = (sized->baseline - static_cast<float>(gi.offsetY)) * s;
  g.width = rec.width * s;
  g.height = rec.height * s;
  g.src = rec;
  g.atlas = &sized->font.texture;
  return g;
}

float FontStore::Advance(Face face, int codepoint, float emPx) {
  const Glyph g = GetGlyph(face, codepoint, emPx);
  return g.present ? g.advance : 0.0f;
}

}  // namespace sb
