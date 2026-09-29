// Color: a vec3 that is a colour.
//
// **An alias, not a struct.** Complex had to be a struct of its own because its
// algebra differs from vec2's -- `z * w` is not a component-by-component
// product. A colour has no algebra of its own: adding two is adding light,
// scaling one is dimming it, multiplying two is a filter, and `mix`, `clamp`,
// `pow` and every other built-in already mean the right thing. What makes a
// colour different is only what it *means*, and meaning is exactly what the
// app reads off the declared type. So the shader is told `#define Color vec3`
// and nothing else, and `.rgb`, swizzles and every built-in come for nothing.
//
// The rule this is a case of: **a new struct when the algebra differs; an alias
// when only the meaning does.**
//
// It is written out for the shader only when a box mentions it, exactly as an
// algebra is. No raylib here, and none in any type.
#pragma once

#include <string>

namespace sb::types::color {

// How the type is spelled.
inline constexpr const char* kName = "Color";

// The GLSL that makes the type exist: the alias, and `hsv(h, s, v)` for making
// one from hue, saturation and value -- hue in turns, so 0.5 is the far side of
// the circle.
std::string GlslPrologue();

// A colour the app chose, for a declaration that named none: deterministic in
// the name, like every other example, and a saturated, bright one so that it is
// visibly *a* colour rather than a grey.
void ExampleValue(const std::string& name, float rgb[3]);

// The same conversions on this side, for the windows that draw one. Hue in
// turns, [0, 1).
void RgbToHsv(const float rgb[3], float hsv[3]);
void HsvToRgb(const float hsv[3], float rgb[3]);

}  // namespace sb::types::color
