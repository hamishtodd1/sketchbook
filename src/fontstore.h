// Font registry for the sketchbook.
//
// The math faces are the KaTeX distribution of Computer Modern, which is the
// same glyph set TeX itself uses -- including the Size1..Size4 faces that hold
// progressively larger delimiters and big operators. Code is set in JetBrains
// Mono.
//
// raylib rasterises a font atlas at one fixed pixel size, so drawing a 16px
// atlas blown up to 400px looks like mush. Because this is an infinitely
// zoomable canvas, the store keeps a small cache of atlases on a size ladder
// and hands back the rung nearest the size actually being drawn, so glyphs stay
// crisp at every zoom level.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "raylib.h"

namespace sb {

enum class Face : int {
  MainRegular = 0,  // upright roman: digits, operators, uppercase Greek
  MainBold,
  MainItalic,
  MathItalic,       // math variables and lowercase Greek
  AMS,              // extra symbols
  Caligraphic,      // \mathcal
  Size1,            // larger delimiters / text-style big operators
  Size2,            // display-style big operators
  Size3,
  Size4,            // largest delimiters
  Mono,             // code
  Count
};
inline constexpr int kFaceCount = static_cast<int>(Face::Count);

// A glyph resolved and scaled to a requested em size, ready to blit.
struct Glyph {
  bool             present = false;  // false => this face has no such codepoint
  float            advance = 0.0f;   // pen advance, px
  float            bearingX = 0.0f;  // left side bearing, px
  float            top = 0.0f;       // baseline -> top of bitmap, px (up = +)
  float            width = 0.0f;
  float            height = 0.0f;
  Rectangle        src{};            // rect within the atlas texture
  const Texture2D* atlas = nullptr;
};

// Font-wide metrics, in em units, read from the TTF itself.
struct FaceMetrics {
  int   unitsPerEm = 1000;
  int   hheaAscent = 0;
  int   hheaDescent = 0;
  float ascent = 0.8f;   // em
  float descent = 0.2f;  // em, positive downwards
};

class FontStore {
 public:
  FontStore() = default;
  ~FontStore();
  FontStore(const FontStore&) = delete;
  FontStore& operator=(const FontStore&) = delete;

  // Reads every face from `fontDir`. Returns false and fills `error` if a
  // required file is missing or unparseable.
  bool Load(const std::string& fontDir, std::string* error);
  void Unload();

  // Call once at the top of a frame. Everything used after it is off limits to
  // the evictor until the next one -- see EvictIfNeeded.
  void BeginFrame() { frameMark_ = clock_; }

  // True if the face's cmap actually covers `codepoint`. Cheap; no rasterising.
  bool HasGlyph(Face face, int codepoint) const;

  // Resolves a glyph at `emPx`. Rasterises a new atlas if no cached rung is
  // close enough. `present` is false when the face lacks the codepoint.
  Glyph GetGlyph(Face face, int codepoint, float emPx);

  const FaceMetrics& Metrics(Face face) const { return metrics_[static_cast<int>(face)]; }

  // Advance width only -- skips the atlas blit bookkeeping when measuring.
  float Advance(Face face, int codepoint, float emPx);

 private:
  // One rasterised atlas: a face pinned to a single em pixel size.
  struct Sized {
    Font                             font{};
    float                            emPx = 0.0f;
    float                            baseline = 0.0f;  // atlas top -> baseline, px
    std::unordered_map<int, int>     index;            // codepoint -> glyph slot
    std::uint64_t                    lastUsed = 0;
    std::size_t                      texels = 0;
  };

  struct FaceData {
    std::vector<unsigned char>    file;      // kept for re-rasterising
    std::unordered_set<int>       coverage;  // codepoints present in the cmap
    std::vector<int>              codepoints;  // sorted, what we rasterise
    bool                          loaded = false;
  };

  Sized* Acquire(Face face, float emPx);
  void   EvictIfNeeded();

  FaceData                                     faces_[kFaceCount];
  FaceMetrics                                  metrics_[kFaceCount];
  std::unordered_map<std::uint64_t, Sized>     cache_;  // key: face<<32 | rung
  std::uint64_t                                clock_ = 0;
  // The clock at the top of this frame. An atlas touched since then is in use
  // by something already drawn or about to be, so it cannot be thrown away.
  std::uint64_t                                frameMark_ = 0;
  std::size_t                                  texelsInUse_ = 0;
};

// ---------------------------------------------------------------------------
// TeX font parameters for Computer Modern, in em units. These are the sigma/xi
// values from the original TeX font metrics; using them is what makes the
// output actually look like LaTeX rather than merely approximate it.
namespace texparams {
inline constexpr float kAxisHeight           = 0.250f;  // sigma22
inline constexpr float kXHeight              = 0.431f;  // sigma5
inline constexpr float kDefaultRuleThickness = 0.040f;  // xi8
inline constexpr float kNum1                 = 0.677f;  // sigma8
inline constexpr float kNum2                 = 0.394f;  // sigma9
inline constexpr float kNum3                 = 0.444f;  // sigma10
inline constexpr float kDenom1               = 0.686f;  // sigma11
inline constexpr float kDenom2               = 0.345f;  // sigma12
inline constexpr float kSup1                 = 0.413f;  // sigma13
inline constexpr float kSup2                 = 0.363f;  // sigma14
inline constexpr float kSup3                 = 0.289f;  // sigma15
inline constexpr float kSub1                 = 0.150f;  // sigma16
inline constexpr float kSub2                 = 0.247f;  // sigma17
inline constexpr float kSupDrop              = 0.386f;  // sigma18
inline constexpr float kSubDrop              = 0.050f;  // sigma19
inline constexpr float kBigOpSpacing1        = 0.111f;  // xi9
inline constexpr float kBigOpSpacing2        = 0.166f;  // xi10
inline constexpr float kBigOpSpacing3        = 0.200f;  // xi11
inline constexpr float kBigOpSpacing4        = 0.600f;  // xi12
inline constexpr float kBigOpSpacing5        = 0.100f;  // xi13
}  // namespace texparams

}  // namespace sb
