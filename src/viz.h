// Vizes: how a type is drawn inside a display window.
//
//   **dw** -- a *display window*: a window that shows values on a scale of its
//   own, and knows about scales, panning, zooming, framing, merging and being
//   carried. It knows nothing about what a value looks like.
//
//   **viz** -- how *one type* is drawn in *one kind of dw*. A viz is the user's:
//   it is a box on a canvas, `Viz(Tri, "Space");` and then GLSL functions with
//   standard names (`vizglsl.h` has the list). **The app's own vizes are written
//   the same way**, in the sketches in the repo's `vizes/` folder, and a canvas
//   that writes its own viz for a type and dw has that one instead.
//
// This file is the GPU half of that. `VizGpu` is one viz window compiled: its
// meshes laid down once, and the programs that draw them and answer questions
// about them. `VizValues` is one dw window's values on their way to it, and the
// answers on their way back. `VizStore` is which vizes there are.
//
// **Nothing here waits for the GPU.** A window's values are drawn by one
// instanced call per piece, straight out of a buffer they were uploaded to only
// when they changed; and what the CPU needs back -- where each value is, which
// one the pointer is nearest, what a drag is asking for -- is read on a later
// frame, once a fence says it has been worked out.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mathprogram.h"
#include "raylib.h"
#include "vizglsl.h"

namespace sb {

class CodeBlock;

// Where a model a viz names is looked for, after the built-in vizes' folder: the
// folder of the canvas open now, so a canvas can carry models of its own.
void SetCanvasFolder(const std::string& folder);

// What a dw tells a viz about the frame it is drawn in.
struct VizFrame {
  Matrix  viewProj{};       // the dw's own space to clip space
  Vector3 eye{};            // where the camera is
  Vector2 size{};           // the window, in pixels
  Vector2 pointer{};        // the pointer, in pixels from the window's corner
  Vector3 rayOrigin{};      // and as a ray through the dw's space
  Vector3 rayDir{};
  float   pixel = 0.0f;         // a pixel of chrome, in the dw's units, in a flat dw
  // The walls, for a dw that has them: what falls outside is not drawn.
  bool    cut = false;
  Vector3 limitsMin{};
  Vector3 limitsMax{};
};

// One viz window, compiled.
class VizGpu {
 public:
  // Compiles the three programs and runs `mesh`. False, with the driver's log,
  // if a program would not compile.
  bool Build(const vizglsl::Sources& sources, std::string* log);
  // By hand, like every GPU resource here.
  void Release();

  bool Ready() const { return compute_ != 0 && draw_.id != 0; }
  // Numbers per value as a dw hands them over: its colour, then the value.
  int  Stride() const { return vizglsl::kTintFloats + valueFloats_; }
  int  ValueFloats() const { return valueFloats_; }
  bool Grabs() const { return grab_; }
  bool Drags() const { return drag_; }

  // Draws `count` values out of `values`, laid out `Stride()` apart.
  void Draw(unsigned int values, int count, const VizFrame& frame) const;
  // Works out, for every value, where it is and how far the pointer is from it;
  // and, if `dragOne` is a value, what the pointer is asking it to be. Written
  // into `answers` for the caller to read back when it is ready.
  void Ask(unsigned int values, unsigned int answers, int count, const VizFrame& frame,
           int dragOne) const;

 private:
  // Where each uniform is, looked up once: every lookup is a trip to the driver.
  struct Slots {
    int viewProj = -1, eye = -1, size = -1, pointer = -1, rayOrigin = -1, rayDir = -1;
    int stride = -1, count = -1, piece = -1, mode = -1, one = -1;
    int cut = -1, limitsMin = -1, limitsMax = -1, pixel = -1;
  };
  static Slots Find(unsigned int program);
  void         Set(const VizFrame& frame, const Slots& at, int count) const;

  Slots computeAt_;
  Slots drawAt_;

  unsigned int compute_ = 0;
  Shader       draw_{};
  unsigned int mesh_[vizglsl::kMaxPieces] = {};
  int          meshCount_[vizglsl::kMaxPieces] = {};
  int          pieces_ = 0;
  int          valueFloats_ = 1;
  bool         grab_ = false;
  bool         drag_ = false;
};

// One dw window's values, on the GPU, and what the viz says back about them.
class VizValues {
 public:
  // Hands over this frame's run of values. Only uploaded when it differs from
  // the last, which is nearly never while nothing is moving.
  void Upload(const std::vector<float>& run, int stride);
  unsigned int Buffer() const { return values_; }
  int          Count() const { return count_; }

  // Asks where every value is and how far the pointer is from each, unless a
  // question is still in flight -- false then. `dragOne` also asks what a drag
  // wants.
  bool Ask(const VizGpu& viz, const VizFrame& frame, int dragOne);
  // Takes the answers, if they have come back. True when new ones have.
  bool Collect();

  // From the last answers. False until there are some for that value.
  bool  Position(int index, Vector3* out) const;
  // How far the pointer was, in pixels. Huge where the viz does not say.
  float Reach(int index) const;
  // The value a drag asked for, handed over once.
  bool  TakeDrag(std::vector<float>* parts);

  void Release();

 private:
  unsigned int       values_ = 0;
  unsigned int       answers_ = 0;
  int                capacity_ = 0;       // values the buffers have room for
  int                allocated_ = 0;      // numbers the values buffer holds
  int                stride_ = 0;
  int                count_ = 0;
  std::vector<float> uploaded_;

  void*              fence_ = nullptr;
  int                askedCount_ = 0;
  int                askedDrag_ = -1;
  int                dragFloats_ = 0;
  std::vector<float> answered_;           // count * 4
  std::vector<float> dragged_;
  bool               haveDrag_ = false;
};

// One thing a dw window hands its viz: a value, by its box and name, or one end
// of one sample of a function being fed.
struct VizItem {
  CodeBlock*  block = nullptr;
  std::string name;
  int         sample = -1;      // -1 for a value
  bool        output = false;   // the answer end of that sample
};

// Everything one dw window does with its viz, which is the same in every dw:
// hand over this frame's values, draw them, and ask where they are, how far the
// pointer is from each, and what a drag wants -- and hear the answers back.
class VizPane {
 public:
  // Takes whatever answers have come back. First, every frame.
  void Collect() { values_.Collect(); }
  // Starts this frame's values over.
  void Begin() {
    items_.clear();
    run_.clear();
  }
  // Hands one over: the colour it is drawn in and its numbers. A value with a
  // number that is not finite -- one not yet worked out -- is left out.
  void Add(const VizItem& item, Color tint, const float* parts, int count);
  // Uploads the values if they changed, and asks the questions again if
  // anything their answers depend on has moved: the values, whatever the dw
  // says in `about` (its camera, its size), the pointer being over it, or a
  // drag of item `dragOne`.
  void Settle(const VizGpu& viz, const VizFrame& frame, std::vector<float> about,
              bool pointerOver, int dragOne);
  void Draw(const VizGpu& viz, const VizFrame& frame) const;

  const std::vector<VizItem>& Items() const { return items_; }
  // Whether the viz it was last handed to says what a drag means.
  bool Drags() const { return drags_; }
  // The item standing for a value, or -1.
  int  Find(const CodeBlock* block, const std::string& name) const;
  // The item the pointer is nearest by the viz's own `grab`, within `reach`
  // pixels, of those `accept` takes; -1 if none.
  int  Nearest(float reach, const std::function<bool(const VizItem&)>& accept) const;
  bool  Position(int item, Vector3* out) const { return values_.Position(item, out); }
  float Reach(int item) const { return values_.Reach(item); }
  bool  TakeDrag(std::vector<float>* parts) { return values_.TakeDrag(parts); }
  void  Release() { values_.Release(); }

 private:
  VizValues            values_;
  std::vector<VizItem> items_;
  std::vector<float>   run_;
  std::vector<float>   askedAbout_;
  bool                 drags_ = false;
};

// Which vizes there are: the canvas's own viz boxes, and the built-in ones in
// the repo's vizes sketches. **A canvas's own wins**, so writing one is how a
// canvas takes a type over.
class VizStore {
 public:
  VizStore();
  ~VizStore();

  // The canvas's boxes, and the algebra it works in -- which decides which of
  // the built-in sketches applies.
  void Sync(const std::vector<std::unique_ptr<CodeBlock>>& blocks,
            const clifford::Signature& signature);
  // The viz for a type in a dw, ready to draw, or null. A built-in one is
  // compiled the first time it is asked for, not before: a viz of an algebra
  // type is an algebra compile, and nobody should pay for one they never see.
  const VizGpu* Find(const std::string& type, const std::string& dw);
  // Whether anything says how a type is drawn in a dw -- the canvas or a
  // built-in -- without compiling it. This is what decides which values a dw
  // takes: a dw shows whatever there is a viz of in it, and has no opinion.
  bool Has(const std::string& type, const std::string& dw) const;
  // How the canvas says a type's space is spread out -- `Samples(float, 24);`.
  // Null if nothing on it does, and then a function gets the spread the app
  // was born with.
  const CodeBlock* Sampler(const std::string& type) const;
  // The vizes the functions of these boxes are drawn with, by `Type@Window`:
  // the canvas's own where it has one that compiles, else the built-in -- which
  // is checked the first time a box needs it.
  std::map<std::string, mathprogram::FunctionViz> FunctionVizes(
      const std::vector<std::unique_ptr<CodeBlock>>& blocks);
  void Clear();
  // The built-in boxes' programs, and the one empty vertex array every viz
  // draws through. A destructor cannot free what is on the GPU.
  void ReleaseGpu();

 private:
  struct Library;
  void Load(const std::string& path);

  std::unordered_map<std::string, CodeBlock*>       local_;
  std::unordered_map<std::string, const CodeBlock*> samplers_;
  std::vector<std::unique_ptr<Library>>             libraries_;
  clifford::Signature                               signature_{};
  bool                                              loaded_ = false;
  double                                            checkedAt_ = -1.0;
};

}  // namespace sb
