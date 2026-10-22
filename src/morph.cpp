#include "morph.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace sb::morph {

float Ease(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

namespace {

float Area(const Contour& c) {
  float twice = 0.0f;
  for (std::size_t i = 0; i < c.size(); ++i) {
    const Point& a = c[i];
    const Point& b = c[(i + 1) % c.size()];
    twice += a.x * b.y - b.x * a.y;
  }
  return twice * 0.5f;
}

Point Centroid(const Contour& c) {
  Point sum;
  for (const Point& p : c) { sum.x += p.x; sum.y += p.y; }
  const float n = static_cast<float>(std::max<std::size_t>(c.size(), 1));
  return {sum.x / n, sum.y / n};
}

bool Inside(Point p, const Contour& c) {
  bool in = false;
  for (std::size_t i = 0, j = c.size() - 1; i < c.size(); j = i++) {
    const Point& a = c[i];
    const Point& b = c[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
      in = !in;
    }
  }
  return in;
}

// Every outline the same way round: what is inside an odd number of others is a
// hole and runs backwards, everything else forwards. Fonts already keep this, but
// a rule, a line or an accent band is made here and could come either way.
std::vector<Contour> Oriented(const std::vector<Contour>& contours) {
  std::vector<Contour> out;
  for (const Contour& c : contours) {
    if (c.size() >= 3) out.push_back(c);
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    int depth = 0;
    for (std::size_t j = 0; j < out.size(); ++j) {
      if (j != i && Inside(out[i][0], out[j])) ++depth;
    }
    const bool hole = (depth % 2) == 1;
    if ((Area(out[i]) < 0.0f) != hole) std::reverse(out[i].begin(), out[i].end());
  }
  return out;
}

struct Box {
  float cx = 0.0f, cy = 0.0f, size = 1.0f;

  Point In(Point p) const { return {(p.x - cx) / size, (p.y - cy) / size}; }
  Point Out(Point q) const { return {cx + q.x * size, cy + q.y * size}; }
};

Box BoxOf(const std::vector<Contour>& contours) {
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (const Contour& c : contours) {
    for (const Point& p : c) {
      x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
      x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    }
  }
  if (x0 > x1) return {};
  return {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f, std::max({x1 - x0, y1 - y0, 1e-3f})};
}

// A contour as a closed path: where each corner falls along it, as a fraction
// of the way round.
struct Arc {
  const Contour*     c = nullptr;
  std::vector<float> at;   // at[i]: corner i, in [0, 1)
};

Arc ArcOf(const Contour& c) {
  Arc arc{&c, std::vector<float>(c.size(), 0.0f)};
  float total = 0.0f;
  for (std::size_t i = 0; i < c.size(); ++i) {
    arc.at[i] = total;
    const Point& a = c[i];
    const Point& b = c[(i + 1) % c.size()];
    total += std::hypot(b.x - a.x, b.y - a.y);
  }
  if (total > 0.0f) {
    for (float& u : arc.at) u /= total;
  } else {
    for (std::size_t i = 0; i < c.size(); ++i) arc.at[i] = static_cast<float>(i) / c.size();
  }
  return arc;
}

// The point `u` of the way round; a corner's own fraction gives the corner itself.
Point Sample(const Arc& arc, float u) {
  u -= std::floor(u);
  const std::vector<float>& at = arc.at;
  const std::size_t i = static_cast<std::size_t>(
      std::upper_bound(at.begin(), at.end(), u) - at.begin() - 1);
  const std::size_t j = (i + 1) % at.size();
  const float from = at[i];
  const float to = (j == 0) ? 1.0f : at[j];
  const float f = (to > from) ? (u - from) / (to - from) : 0.0f;
  const Point& a = (*arc.c)[i];
  const Point& b = (*arc.c)[j];
  return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
}

// Two outlines made point-for-point alike: both get every corner of both, each at
// the same fraction of the way round, so each keeps its own shape exactly at its
// end of the journey and nothing slides along an edge in between. Where round
// they start is chosen first, as whichever turn of one against the other puts
// the two shapes closest.
void Pair(const Contour& a, const Contour& b, Contour* outA, Contour* outB) {
  const Arc arcA = ArcOf(a);
  const Arc arcB = ArcOf(b);

  constexpr int kTurns = 64;
  const auto normalised = [](const Arc& arc) {
    std::vector<Point> out(kTurns);
    for (int k = 0; k < kTurns; ++k) out[k] = Sample(arc, static_cast<float>(k) / kTurns);
    const Box box = BoxOf({out});
    for (Point& p : out) p = box.In(p);
    return out;
  };
  const std::vector<Point> na = normalised(arcA);
  const std::vector<Point> nb = normalised(arcB);
  int   bestTurn = 0;
  float bestCost = 1e30f;
  for (int r = 0; r < kTurns; ++r) {
    float cost = 0.0f;
    for (int k = 0; k < kTurns; ++k) {
      const Point& p = na[k];
      const Point& q = nb[(k + r) % kTurns];
      cost += (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y);
    }
    if (cost < bestCost) { bestCost = cost; bestTurn = r; }
  }
  const float shift = static_cast<float>(bestTurn) / kTurns;

  std::vector<float> fractions = arcA.at;
  for (float u : arcB.at) {
    const float v = u - shift;
    fractions.push_back(v - std::floor(v));
  }
  std::sort(fractions.begin(), fractions.end());
  std::vector<float> kept;
  for (float u : fractions) {
    if (kept.empty() || u - kept.back() > 1e-5f) kept.push_back(u);
  }
  if (kept.size() > 1 && 1.0f - kept.back() + kept.front() <= 1e-5f) kept.pop_back();
  outA->clear();
  outB->clear();
  for (float u : kept) {
    outA->push_back(Sample(arcA, u));
    outB->push_back(Sample(arcB, u + shift));
  }
}

// A contour with nothing to become: it shrinks to a point, or grows from one, at
// where its counterpart would be in the other piece.
Contour Dot(Point p, std::size_t n) { return Contour(n, p); }

float Step(float from, float to, float t) { return Ease((t - from) / (to - from)); }

// A contour that has shrunk to nothing draws nothing, rather than a speck.
bool Vanished(const Contour& c) {
  const Box box = BoxOf({c});
  return box.size <= 2e-3f;
}

}  // namespace

Morph::Morph(const std::vector<Piece>& from, const std::vector<Piece>& to) {
  // The k-th piece with a key, on each side.
  std::map<std::string, std::vector<std::size_t>> later;
  for (std::size_t j = 0; j < to.size(); ++j) later[to[j].key].push_back(j);
  std::map<std::string, std::size_t> used;
  std::vector<bool> taken(to.size(), false);

  for (const Piece& piece : from) {
    std::vector<std::size_t>& candidates = later[piece.key];
    std::size_t&              next = used[piece.key];
    Track                     track;
    if (piece.key.empty() || next >= candidates.size()) {
      track.way = Way::Leave;
      track.from = track.to = Oriented(piece.contours);
      tracks_.push_back(std::move(track));
      continue;
    }
    const std::size_t partner = candidates[next++];
    taken[partner] = true;
    ++matched_;

    const std::vector<Contour> a = Oriented(piece.contours);
    const std::vector<Contour> b = Oriented(to[partner].contours);
    const Box boxA = BoxOf(a);
    const Box boxB = BoxOf(b);
    // Outline with outline and hole with hole, nearest first, each placed within
    // its own piece -- so the stem of an `i` finds the stem, and its dot the dot.
    std::vector<bool> doneA(a.size(), false), doneB(b.size(), false);
    struct Option { float cost; std::size_t i, j; };
    std::vector<Option> options;
    for (std::size_t i = 0; i < a.size(); ++i) {
      for (std::size_t j = 0; j < b.size(); ++j) {
        if ((Area(a[i]) < 0.0f) != (Area(b[j]) < 0.0f)) continue;
        const Point p = boxA.In(Centroid(a[i]));
        const Point q = boxB.In(Centroid(b[j]));
        const float sizes = std::log((std::fabs(Area(a[i])) + 1e-6f) / boxA.size / boxA.size) -
                            std::log((std::fabs(Area(b[j])) + 1e-6f) / boxB.size / boxB.size);
        options.push_back(
            {(p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + 0.02f * sizes * sizes, i, j});
      }
    }
    std::sort(options.begin(), options.end(),
              [](const Option& l, const Option& r) { return l.cost < r.cost; });
    for (const Option& option : options) {
      if (doneA[option.i] || doneB[option.j]) continue;
      doneA[option.i] = doneB[option.j] = true;
      Contour ca, cb;
      Pair(a[option.i], b[option.j], &ca, &cb);
      track.from.push_back(std::move(ca));
      track.to.push_back(std::move(cb));
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (doneA[i]) continue;
      track.from.push_back(a[i]);
      track.to.push_back(Dot(boxB.Out(boxA.In(Centroid(a[i]))), a[i].size()));
    }
    for (std::size_t j = 0; j < b.size(); ++j) {
      if (doneB[j]) continue;
      track.from.push_back(Dot(boxA.Out(boxB.In(Centroid(b[j]))), b[j].size()));
      track.to.push_back(b[j]);
    }
    tracks_.push_back(std::move(track));
  }

  for (std::size_t j = 0; j < to.size(); ++j) {
    if (taken[j]) continue;
    Track track;
    track.way = Way::Arrive;
    track.from = track.to = Oriented(to[j].contours);
    tracks_.push_back(std::move(track));
  }
}

std::vector<Shape> Morph::At(float t) const {
  t = std::clamp(t, 0.0f, 1.0f);
  const float s = Ease(t);
  std::vector<Shape> out;
  for (const Track& track : tracks_) {
    Shape shape;
    switch (track.way) {
      case Way::Move: shape.alpha = 1.0f; break;
      // What goes is gone before the moving pieces settle, and what comes only
      // starts once they are under way, so the two are never on top of each other.
      case Way::Leave: shape.alpha = 1.0f - Step(0.0f, 0.6f, t); break;
      case Way::Arrive: shape.alpha = Step(0.4f, 1.0f, t); break;
    }
    if (shape.alpha <= 0.0f) continue;
    for (std::size_t k = 0; k < track.from.size(); ++k) {
      const Contour& a = track.from[k];
      const Contour& b = track.to[k];
      Contour c(a.size());
      for (std::size_t i = 0; i < a.size(); ++i) {
        c[i] = {a[i].x + (b[i].x - a[i].x) * s, a[i].y + (b[i].y - a[i].y) * s};
      }
      if (!Vanished(c)) shape.contours.push_back(std::move(c));
    }
    if (!shape.contours.empty()) out.push_back(std::move(shape));
  }
  return out;
}

}  // namespace sb::morph
