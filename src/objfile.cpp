#include "objfile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sb::obj {
namespace {

struct Corner {
  int position = -1;
  int uv = -1;
  int normal = -1;
};

// `7`, `7/3`, `7//2`, `7/3/2`; negative numbers count back from the end.
bool ReadCorner(const std::string& word, std::size_t positions, std::size_t uvs,
                std::size_t normals, Corner* out) {
  int  index[3] = {0, 0, 0};
  int  part = 0;
  std::size_t at = 0;
  while (at <= word.size() && part < 3) {
    const std::size_t slash = word.find('/', at);
    const std::string piece = word.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
    if (!piece.empty()) index[part] = std::atoi(piece.c_str());
    ++part;
    if (slash == std::string::npos) break;
    at = slash + 1;
  }
  const auto resolve = [](int i, std::size_t count) {
    if (i > 0) return i - 1;
    if (i < 0) return static_cast<int>(count) + i;
    return -1;
  };
  out->position = resolve(index[0], positions);
  out->uv = resolve(index[1], uvs);
  out->normal = resolve(index[2], normals);
  return out->position >= 0 && out->position < static_cast<int>(positions);
}

}  // namespace

bool Load(const std::string& path, std::vector<float>* corners, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error != nullptr) *error = "there is no file " + path;
    return false;
  }
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<float, 2>> uvs;
  std::vector<std::array<float, 3>> normals;
  std::vector<Corner>               triangles;   // three a triangle

  std::string line;
  while (std::getline(file, line)) {
    std::istringstream words(line);
    std::string        kind;
    words >> kind;
    if (kind == "v") {
      std::array<float, 3> p{};
      words >> p[0] >> p[1] >> p[2];
      positions.push_back(p);
    } else if (kind == "vt") {
      std::array<float, 2> t{};
      words >> t[0] >> t[1];
      uvs.push_back(t);
    } else if (kind == "vn") {
      std::array<float, 3> n{};
      words >> n[0] >> n[1] >> n[2];
      normals.push_back(n);
    } else if (kind == "f") {
      std::vector<Corner> face;
      std::string         word;
      while (words >> word) {
        Corner corner;
        if (ReadCorner(word, positions.size(), uvs.size(), normals.size(), &corner)) {
          face.push_back(corner);
        }
      }
      // Fanned from the first corner.
      for (std::size_t i = 1; i + 1 < face.size(); ++i) {
        triangles.push_back(face[0]);
        triangles.push_back(face[i]);
        triangles.push_back(face[i + 1]);
      }
    }
  }
  if (triangles.empty()) {
    if (error != nullptr) *error = path + " has no faces";
    return false;
  }

  // Smooth normals where the file gives none: every face's normal, weighted by
  // its area, added into each of its corners' positions.
  std::vector<std::array<float, 3>> smooth(positions.size(), {0.0f, 0.0f, 0.0f});
  for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
    const auto& a = positions[static_cast<std::size_t>(triangles[t].position)];
    const auto& b = positions[static_cast<std::size_t>(triangles[t + 1].position)];
    const auto& c = positions[static_cast<std::size_t>(triangles[t + 2].position)];
    const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    const float n[3] = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
    for (int k = 0; k < 3; ++k) {
      auto& s = smooth[static_cast<std::size_t>(triangles[t + static_cast<std::size_t>(k)].position)];
      s[0] += n[0];
      s[1] += n[1];
      s[2] += n[2];
    }
  }

  // Centred on the middle of its bounds and scaled so its furthest point is
  // one unit out.
  std::array<float, 3> lo = positions.front();
  std::array<float, 3> hi = positions.front();
  for (const auto& p : positions) {
    for (int k = 0; k < 3; ++k) {
      lo[static_cast<std::size_t>(k)] = std::min(lo[static_cast<std::size_t>(k)], p[static_cast<std::size_t>(k)]);
      hi[static_cast<std::size_t>(k)] = std::max(hi[static_cast<std::size_t>(k)], p[static_cast<std::size_t>(k)]);
    }
  }
  const float middle[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
  float reach = 0.0f;
  for (const auto& p : positions) {
    reach = std::max(reach, std::hypot(p[0] - middle[0], p[1] - middle[1], p[2] - middle[2]));
  }
  const float scale = reach > 0.0f ? 1.0f / reach : 1.0f;

  corners->clear();
  corners->reserve(triangles.size() * 8);
  for (const Corner& corner : triangles) {
    const auto& p = positions[static_cast<std::size_t>(corner.position)];
    float n[3];
    if (corner.normal >= 0 && corner.normal < static_cast<int>(normals.size())) {
      const auto& given = normals[static_cast<std::size_t>(corner.normal)];
      n[0] = given[0];
      n[1] = given[1];
      n[2] = given[2];
    } else {
      const auto& s = smooth[static_cast<std::size_t>(corner.position)];
      n[0] = s[0];
      n[1] = s[1];
      n[2] = s[2];
    }
    const float length = std::max(std::hypot(n[0], n[1], n[2]), 1e-12f);
    const bool  hasUv = corner.uv >= 0 && corner.uv < static_cast<int>(uvs.size());
    corners->insert(corners->end(),
                    {(p[0] - middle[0]) * scale, (p[1] - middle[1]) * scale,
                     (p[2] - middle[2]) * scale, n[0] / length, n[1] / length, n[2] / length,
                     hasUv ? uvs[static_cast<std::size_t>(corner.uv)][0] : 0.0f,
                     hasUv ? uvs[static_cast<std::size_t>(corner.uv)][1] : 0.0f});
  }
  return true;
}

}  // namespace sb::obj
