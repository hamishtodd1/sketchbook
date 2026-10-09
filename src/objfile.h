// Reading a model a viz draws, from a Wavefront OBJ file.
//
// A viz's geometry is usually laid down by its own GLSL, but some shapes are not
// worth writing out by hand -- a pair of cupped hands -- and are better made in
// a modelling program. So a piece of a viz can name a file instead, and this
// reads it. Raylib-free, like everything else that talks to the outside world.
#pragma once

#include <string>
#include <vector>

namespace sb::obj {

// The model as triangles, eight numbers a corner -- position, normal, texture
// coordinate -- which is how a viz's mesh is laid out. It is **centred and
// scaled to fit the unit sphere**, the size of the sphere a viz would lay down
// itself, so `vert` sizes it the same way whatever units it was made in. Faces of
// more than three corners are fanned into triangles, and a model with no normals
// is given smooth ones. False, with why, if the file cannot be read or has no
// faces.
bool Load(const std::string& path, std::vector<float>* corners, std::string* error);

}  // namespace sb::obj
