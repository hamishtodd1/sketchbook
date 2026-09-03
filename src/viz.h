// Vizes: how a type is drawn inside a display window.
//
// The vocabulary, since it is new and the whole of this file depends on it:
//
//   **dw** -- a *display window*. A window that shows values on a scale of its
//   own: the number line and the plot. `AxisPanel` and `PlotPanel` are the two
//   there are. A dw knows about scales, panning, zooming, merging and being
//   carried; it knows nothing about what a real or a function looks like.
//
//   **Viz** -- how *one type* is drawn inside a dw. `RealViz` is how a real
//   looks on a number line, `RealFunctionViz` is how a function looks on a
//   plot, and `ScalarBivectorViz` is one of the ones still to come. A viz is
//   what the app has been holding as hard-coded drawing code, and it is what is
//   meant to become the user's to write.
//
// A viz answers four questions, and all four are eventually the user's to
// answer, in boxes on the canvas:
//
//   1. **mesh**      -- its geometry, in its own units
//   2. **place**     -- where each of those vertices ends up, given the value
//   3. **distance**  -- how near the pointer counts as being on it
//   4. **drag**      -- what value the pointer is now asking for
//
// The first two are what this file carries. A box saying `Viz(Real);` supplies
// one of them: its C++ half lays down a mesh with `vertex(x, y)` and prints it
// back, and its GLSL half is the vertex shader that places those vertices. Two
// boxes, paired by the type they name -- which needs no new language rule,
// since a box is already C++ first and GLSL if C++ refuses it.
//
// A type with no viz boxes on the canvas keeps the drawing the app was born
// with. Nothing has to be authored for a canvas to work; authoring one is how
// you take it over.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mathprogram.h"
#include "raylib.h"

namespace sb {

class CodeBlock;

// One type's viz, as gathered from the boxes on the canvas.
struct Viz {
  // The example mesh: what the viz box drew standing alone, for an example
  // value of its type. Used before any real value has one, and for a type whose
  // shape does not depend on its value at all.
  const std::vector<float>* mesh = nullptr;
  const std::vector<float>* lines = nullptr;
  // The vertex shader that places it, and the slots the window fills in.
  Shader shader{};
  bool   haveShader = false;
  int    valueLoc = -1;
  int    areaLoc = -1;
  int    viewLoc = -1;
  int    scaleLoc = -1;

  // The shader has arrived, so this viz can place a mesh if it is given one.
  bool Ready() const { return haveShader; }

  // Draws one value's geometry. `area` is the rectangle the dw plots in, its
  // middle at the view's centre; `view` is what that rectangle spans, as
  // (centre.x, centre.y, wide, high). Both are the shader's to use or ignore.
  // `scale` is how big chrome is drawn at the current zoom.
  void Draw(const std::vector<float>& tris, const std::vector<float>& segments, float value,
            Rectangle area, Vector4 view, float scale, Color tint) const;
};

// Every viz the canvas currently defines. Rebuilt from the boxes each frame it
// might have changed -- a viz is not a thing you register, it is a thing that
// is written down somewhere on the canvas and therefore true.
class VizStore {
 public:
  // True if what the canvas says has changed, in which case every box using one
  // of these types has to be rebuilt: a viz's lines are part of their programs.
  bool Sync(const std::vector<std::unique_ptr<CodeBlock>>& blocks);
  // What to inject into those programs, by type.
  const mathprogram::VizSources& Sources() const { return sources_; }
  const Viz* Find(const std::string& type) const;
  void       Clear() { vizes_.clear(); }

 private:
  std::map<std::string, Viz>    vizes_;
  mathprogram::VizSources       sources_;
};

}  // namespace sb
