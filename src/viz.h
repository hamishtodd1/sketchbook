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
// **A type may have more than one viz, in more than one window.** A function is
// a curve on a plot and a field of colour on a number line, and those are two
// different answers to "how is one drawn" rather than one answer used twice. So
// a viz box may name the window it is for -- `Viz(RToR, Axis)` -- and
// the two are held apart under `Type@Window`. A box that names no window is the
// viz a window gets when it asks for a type and nothing more.
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
  // The box whose half lays this viz's geometry down. Its compute shader is run
  // for the value being drawn -- which is why the mesh is not stored here: it
  // belongs to the value, and there is a different one for every value.
  const CodeBlock* meshBlock = nullptr;
  // The vertex shader that places it, and the slots the window fills in.
  Shader shader{};
  bool   haveShader = false;
  int    valueLoc = -1;
  int    areaLoc = -1;
  int    viewLoc = -1;
  int    scaleLoc = -1;
  int    tintLoc = -1;

  // The shader has arrived, so this viz can place a mesh if it is given one.
  bool Ready() const { return haveShader && meshBlock != nullptr; }

  // Draws one value's geometry. `area` is the rectangle the dw plots in, its
  // middle at the view's centre; `view` is what that rectangle spans, as
  // (centre.x, centre.y, wide, high). Both are the shader's to use or ignore.
  // `scale` is how big chrome is drawn at the current zoom.
  void Draw(float value, Rectangle area, Vector4 view, float scale, Color tint) const;
};

// Every viz the canvas currently defines. Rebuilt from the boxes each frame it
// might have changed -- a viz is not a thing you register, it is a thing that
// is written down somewhere on the canvas and therefore true.
class VizStore {
 public:
  void Sync(const std::vector<std::unique_ptr<CodeBlock>>& blocks);
  // `window` names which of a type's vizes is wanted; empty asks for the one
  // that named no window.
  const Viz* Find(const std::string& type, const std::string& window = {}) const;
  // How the canvas says a type's space is spread out -- `Samples(float, 24);`.
  // A function is shown by being fed, and this is where the food comes from.
  // Null if nothing on the canvas has anything to say about that type, in which
  // case the spread the app was born with is what a function gets.
  const CodeBlock* Sampler(const std::string& type) const;
  void       Clear() { vizes_.clear(); }
  // The one empty vertex array every viz draws through. A destructor cannot
  // free what is on the GPU, so it goes by hand.
  static void ReleaseGpu();

 private:
  std::map<std::string, Viz>             vizes_;
  std::map<std::string, const CodeBlock*> samplers_;
};

}  // namespace sb
