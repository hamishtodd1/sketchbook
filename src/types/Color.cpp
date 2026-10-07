#include "types/Color.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sb::types::color {

std::string GlslPrologue() {
  return
      "#define Color vec3\n"
      "Color hsv(float h, float s, float v) {\n"
      "  vec3 k = clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);\n"
      "  return v * mix(vec3(1.0), k, s);\n"
      "}\n"
      // And back: hue in turns, saturation, value.
      "vec3 hsvOf(Color c) {\n"
      "  float high = max(c.r, max(c.g, c.b));\n"
      "  float range = high - min(c.r, min(c.g, c.b));\n"
      "  float h = 0.0;\n"
      "  if (range > 0.000001) {\n"
      "    if (high == c.r) h = mod((c.g - c.b) / range, 6.0);\n"
      "    else if (high == c.g) h = (c.b - c.r) / range + 2.0;\n"
      "    else h = (c.r - c.g) / range + 4.0;\n"
      "  }\n"
      "  return vec3(h / 6.0, high > 0.000001 ? range / high : 0.0, high);\n"
      "}\n";
}

void ExampleValue(const std::string& name, float rgb[3]) {
  std::uint32_t seed = 2166136261u;
  for (char c : name) {
    seed ^= static_cast<unsigned char>(c);
    seed *= 16777619u;
  }
  const float hsv[3] = {static_cast<float>(seed % 1000u) / 1000.0f, 0.7f, 0.9f};
  HsvToRgb(hsv, rgb);
}

void RgbToHsv(const float rgb[3], float hsv[3]) {
  const float r = rgb[0], g = rgb[1], b = rgb[2];
  const float high = std::max({r, g, b});
  const float low = std::min({r, g, b});
  const float range = high - low;
  float       hue = 0.0f;
  if (range > 1e-6f) {
    if (high == r) {
      hue = (g - b) / range;
    } else if (high == g) {
      hue = 2.0f + (b - r) / range;
    } else {
      hue = 4.0f + (r - g) / range;
    }
    hue /= 6.0f;
    hue -= std::floor(hue);
  }
  hsv[0] = hue;
  hsv[1] = (std::fabs(high) > 1e-6f) ? range / high : 0.0f;
  hsv[2] = high;
}

void HsvToRgb(const float hsv[3], float rgb[3]) {
  // The same formula the shader's `hsv` uses, so the two sides agree exactly.
  const float offsets[3] = {0.0f, 4.0f, 2.0f};
  for (int i = 0; i < 3; ++i) {
    const float k = std::clamp(
        std::fabs(std::fmod(hsv[0] * 6.0f + offsets[i] + 600.0f, 6.0f) - 3.0f) - 1.0f, 0.0f,
        1.0f);
    rgb[i] = hsv[2] * (1.0f + hsv[1] * (k - 1.0f));
  }
}

}  // namespace sb::types::color
