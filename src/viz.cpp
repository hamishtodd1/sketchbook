#include "viz.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "blocks.h"
#include "glext.h"
#include "logging.h"
#include "objfile.h"
#include "rlgl.h"
#include "types/examples.h"

#ifndef SKETCHBOOK_VIZES_DIR
#define SKETCHBOOK_VIZES_DIR "vizes"
#endif

namespace sb {

namespace {

// One empty vertex array, shared by every viz. A core-profile draw needs some
// vertex array bound even when it reads nothing from one, and this is that
// nothing: the geometry arrives through a storage buffer instead.
unsigned int g_emptyVao = 0;

std::string g_canvasFolder;

// A model named by a viz: beside the built-in vizes, or beside the canvas.
std::string FindModel(const std::string& name) {
  const std::filesystem::path given(name);
  std::error_code ec;
  if (given.is_absolute() && std::filesystem::exists(given, ec)) return name;
  for (const std::filesystem::path& folder :
       {std::filesystem::path(VizesFolder()), std::filesystem::path(g_canvasFolder)}) {
    if (folder.empty()) continue;
    const std::filesystem::path there = folder / given;
    if (std::filesystem::exists(there, ec)) return there.string();
  }
  return name;
}

void SetInt(int at, int value) {
  if (at >= 0) rlSetUniform(at, &value, RL_SHADER_UNIFORM_INT, 1);
}
void SetFloat(int at, float value) {
  if (at >= 0) rlSetUniform(at, &value, RL_SHADER_UNIFORM_FLOAT, 1);
}
void SetVec2(int at, Vector2 value) {
  const float v[2] = {value.x, value.y};
  if (at >= 0) rlSetUniform(at, v, RL_SHADER_UNIFORM_VEC2, 1);
}
void SetVec3(int at, Vector3 value) {
  const float v[3] = {value.x, value.y, value.z};
  if (at >= 0) rlSetUniform(at, v, RL_SHADER_UNIFORM_VEC3, 1);
}

}  // namespace

void SetCanvasFolder(const std::string& folder) { g_canvasFolder = folder; }

namespace {
std::string g_vizesFolder = SKETCHBOOK_VIZES_DIR;
}  // namespace
void               SetVizesFolder(const std::string& folder) { g_vizesFolder = folder; }
const std::string& VizesFolder() { return g_vizesFolder; }

// ------------------------------------------------------------------- VizGpu

VizGpu::Slots VizGpu::Find(unsigned int program) const {
  const auto at = [program](const char* name) {
    return static_cast<int>(rlGetLocationUniform(program, name));
  };
  Slots s;
  s.viewProj = at("sbViewProj");
  s.eye = at("sbEye");
  s.size = at("sbSize");
  s.pointer = at("sbPointer");
  s.rayOrigin = at("sbRayOrigin");
  s.rayDir = at("sbRayDir");
  s.stride = at("sbStride");
  s.count = at("sbCount");
  s.piece = at("sbPiece");
  s.mode = at("sbMode");
  s.one = at("sbOne");
  s.cut = at("sbCut");
  s.limitsMin = at("sbLimitsMin");
  s.limitsMax = at("sbLimitsMax");
  s.pixel = at("sbPixel");
  s.seed = at("sbSeed");
  EachKnob([&](const mathprogram::Literal& knob) {
    const int where = at(knob.uniform.c_str());
    if (where >= 0) s.knobs.emplace_back(knob.uniform, where);
  });
  return s;
}

void VizGpu::EachKnob(const std::function<void(const mathprogram::Literal&)>& visit) const {
  if (program_ == nullptr) return;
  for (const mathprogram::Literal& knob : program_->imported()) visit(knob);
  for (const mathprogram::Variable& variable : program_->variables()) {
    for (const mathprogram::Literal& knob : variable.literals) visit(knob);
  }
}

void VizGpu::SetKnobs(const Slots& at) const {
  if (at.knobs.empty()) return;
  EachKnob([&](const mathprogram::Literal& knob) {
    for (const auto& [uniform, where] : at.knobs) {
      if (uniform != knob.uniform) continue;
      if (knob.array) {
        rlSetUniform(where, knob.components.data(), RL_SHADER_UNIFORM_FLOAT,
                     static_cast<int>(knob.components.size()));
      } else if (knob.components.size() == 3) {
        rlSetUniform(where, knob.components.data(), RL_SHADER_UNIFORM_VEC3, 1);
      } else {
        rlSetUniform(where, &knob.value, RL_SHADER_UNIFORM_FLOAT, 1);
      }
    }
  });
}

std::vector<float> VizGpu::Knobs() const {
  std::vector<float> out;
  EachKnob([&](const mathprogram::Literal& knob) {
    if (knob.components.empty()) out.push_back(knob.value);
    else out.insert(out.end(), knob.components.begin(), knob.components.end());
  });
  return out;
}

void VizGpu::Mesh(int k) {
  const unsigned int zero = 0;
  rlUpdateShaderBuffer(mesh_[k], &zero, sizeof(zero), 0);
  SetInt(computeAt_.mode, 0);
  SetInt(computeAt_.piece, k);
  SetKnobs(computeAt_);
  rlBindShaderBuffer(mesh_[k], 1);
  rlComputeShaderDispatch(1, 1, 1);
  glext::Barrier(glext::kShaderStorage | glext::kBufferUpdate);
  unsigned int laid = 0;
  rlReadShaderBuffer(mesh_[k], &laid, sizeof(laid), 0);
  const int kept = std::min(static_cast<int>(laid), vizglsl::kMeshCap);
  meshCount_[k] = kept - kept % 3;
}

bool VizGpu::Example(unsigned int seed, const VizFrame& frame, std::vector<float>* out) const {
  if (!Ready() || !example_ || valueFloats_ <= 0) return false;
  const unsigned int bytes = static_cast<unsigned int>(valueFloats_ * sizeof(float));
  const unsigned int buffer = rlLoadShaderBuffer(bytes, nullptr, RL_DYNAMIC_COPY);
  rlEnableShader(compute_);
  Set(frame, computeAt_, 0);
  SetInt(computeAt_.mode, 3);
  SetInt(computeAt_.seed, static_cast<int>(seed));
  rlBindShaderBuffer(buffer, 3);
  rlComputeShaderDispatch(1, 1, 1);
  glext::Barrier(glext::kShaderStorage | glext::kBufferUpdate);
  out->assign(static_cast<std::size_t>(valueFloats_), 0.0f);
  rlReadShaderBuffer(buffer, out->data(), bytes, 0);
  rlDisableShader();
  rlUnloadShaderBuffer(buffer);
  return std::all_of(out->begin(), out->end(), [](float v) { return std::isfinite(v); });
}

void VizGpu::Refresh() {
  if (!Ready() || program_ == nullptr) return;
  std::vector<float> now = Knobs();
  if (now == meshedWith_) return;
  // **A knob never recompiles**, but a mesh made with it is made again: one
  // dispatch a piece, only when a knob has moved.
  meshedWith_ = std::move(now);
  rlEnableShader(compute_);
  for (int k = 0; k < pieces_; ++k) {
    if (!model_[k]) Mesh(k);
  }
  rlDisableShader();
}

bool VizGpu::Build(const vizglsl::Sources& sources, const mathprogram::Program* program,
                   std::string* log) {
  Release();
  log->clear();
  program_ = program;
  example_ = sources.example;
  pieces_ = sources.pieces;
  valueFloats_ = sources.stride;
  grab_ = sources.grab;
  drag_ = sources.drag;

  // Each program the lean way first, if there is one: only the structs of the
  // algebra's types, which is all a viz that reads a value's parts needs, and a
  // fraction of the compile. The whole algebra only if that will not do -- and
  // then its log is the one reported, since it is the one that means something.
  const auto compute = [&](const std::string& source, std::string* into) {
    const log::Catch   caught(into);
    const unsigned int shader = rlCompileShader(source.c_str(), RL_COMPUTE_SHADER);
    return shader != 0 ? rlLoadComputeShaderProgram(shader) : 0u;
  };
  const auto draw = [&](const std::string& vertex, const std::string& fragment,
                        std::string* into) {
    Shader shader{};
    {
      const log::Catch caught(into);
      shader = LoadShaderFromMemory(vertex.c_str(), fragment.c_str());
    }
    if (!IsShaderValid(shader)) {
      if (shader.id != 0) UnloadShader(shader);
      return Shader{};
    }
    return shader;
  };
  std::string ignored;
  if (sources.lean) compute_ = compute(sources.leanCompute, &ignored);
  if (compute_ == 0) compute_ = compute(sources.compute, log);
  if (compute_ == 0) return false;
  if (sources.lean) draw_ = draw(sources.leanVertex, sources.leanFragment, &ignored);
  if (draw_.id == 0) draw_ = draw(sources.vertex, sources.fragment, log);
  if (draw_.id == 0) {
    Release();
    return false;
  }
  log->clear();
  computeAt_ = Find(compute_);
  drawAt_ = Find(draw_.id);

  // **The geometry is laid down once**, here, and not again until the window is
  // edited: it does not depend on the value, which arrives in `vert`. The one
  // thing read back is how much of it there is, once per build.
  const unsigned int bytes = static_cast<unsigned int>(
      sizeof(unsigned int) + static_cast<std::size_t>(vizglsl::kMeshCap) *
                                 vizglsl::kVertexFloats * sizeof(float));
  rlEnableShader(compute_);
  for (int k = 0; k < pieces_; ++k) {
    // A model is read from its file into a buffer the size of it, in the same
    // layout `mesh` writes: the count, then eight numbers a corner.
    if (!sources.model[k].empty()) {
      std::vector<float> corners;
      std::string        why;
      if (!obj::Load(FindModel(sources.model[k]), &corners, &why)) {
        rlDisableShader();
        Release();
        *log = "could not read the model: " + why;
        return false;
      }
      const unsigned int count = static_cast<unsigned int>(corners.size() / vizglsl::kVertexFloats);
      const unsigned int size =
          static_cast<unsigned int>(sizeof(unsigned int) + corners.size() * sizeof(float));
      mesh_[k] = rlLoadShaderBuffer(size, nullptr, RL_DYNAMIC_COPY);
      rlUpdateShaderBuffer(mesh_[k], &count, sizeof(count), 0);
      rlUpdateShaderBuffer(mesh_[k], corners.data(),
                           static_cast<unsigned int>(corners.size() * sizeof(float)),
                           sizeof(unsigned int));
      meshCount_[k] = static_cast<int>(count - count % 3);
      model_[k] = true;
      continue;
    }
    mesh_[k] = rlLoadShaderBuffer(bytes, nullptr, RL_DYNAMIC_COPY);
    Mesh(k);
  }
  rlDisableShader();
  meshedWith_ = Knobs();
  return true;
}

void VizGpu::Release() {
  if (compute_ != 0) rlUnloadShaderProgram(compute_);
  compute_ = 0;
  if (draw_.id != 0) UnloadShader(draw_);
  draw_ = Shader{};
  for (int k = 0; k < vizglsl::kMaxPieces; ++k) {
    if (mesh_[k] != 0) rlUnloadShaderBuffer(mesh_[k]);
    mesh_[k] = 0;
    meshCount_[k] = 0;
    model_[k] = false;
  }
  pieces_ = 0;
  meshedWith_.clear();
}

void VizGpu::Set(const VizFrame& frame, const Slots& at, int count) const {
  if (at.viewProj >= 0) rlSetUniformMatrix(at.viewProj, frame.viewProj);
  SetVec3(at.eye, frame.eye);
  SetVec2(at.size, frame.size);
  SetVec2(at.pointer, frame.pointer);
  SetVec3(at.rayOrigin, frame.rayOrigin);
  SetVec3(at.rayDir, frame.rayDir);
  SetInt(at.stride, Stride());
  SetInt(at.count, count);
  SetFloat(at.cut, frame.cut ? 1.0f : 0.0f);
  SetVec3(at.limitsMin, frame.limitsMin);
  SetVec3(at.limitsMax, frame.limitsMax);
  SetFloat(at.pixel, frame.pixel);
  SetKnobs(at);
}

void VizGpu::Draw(unsigned int values, int count, const VizFrame& frame) const {
  if (!Ready() || values == 0 || count <= 0) return;

  // Whatever raylib had queued goes first: this draw goes round its batch.
  rlDrawRenderBatchActive();
  rlEnableShader(draw_.id);
  Set(frame, drawAt_, count);
  rlBindShaderBuffer(values, 2);

  // A flat shape has no back face, and a mesh written the way anyone would
  // write one should not vanish because the camera or the screen's upside-down
  // y turned it round. Nobody authoring a viz should have to know about winding.
  rlDisableBackfaceCulling();
  if (g_emptyVao == 0) g_emptyVao = rlLoadVertexArray();
  rlEnableVertexArray(g_emptyVao);
  for (int k = 0; k < pieces_; ++k) {
    if (meshCount_[k] < 3) continue;
    rlBindShaderBuffer(mesh_[k], 1);
    SetInt(drawAt_.piece, k);
    glext::DrawArraysInstanced(glext::kTriangles, 0, meshCount_[k], count);
  }
  rlDisableVertexArray();
  rlEnableBackfaceCulling();
  rlDisableShader();
}

void VizGpu::Ask(unsigned int values, unsigned int answers, int count, const VizFrame& frame,
                 int dragOne) const {
  if (!Ready() || values == 0 || answers == 0 || count <= 0) return;
  rlEnableShader(compute_);
  Set(frame, computeAt_, count);
  rlBindShaderBuffer(values, 2);
  rlBindShaderBuffer(answers, 3);
  SetInt(computeAt_.mode, 1);
  rlComputeShaderDispatch(static_cast<unsigned int>(count), 1, 1);
  if (drag_ && dragOne >= 0 && dragOne < count) {
    SetInt(computeAt_.mode, 2);
    SetInt(computeAt_.one, dragOne);
    rlComputeShaderDispatch(1, 1, 1);
  }
  glext::Barrier(glext::kShaderStorage | glext::kBufferUpdate);
  rlDisableShader();
}

// ---------------------------------------------------------------- VizValues

void VizValues::Upload(const std::vector<float>& run, int stride) {
  if (stride <= 0) return;
  if (stride == stride_ && run == uploaded_) return;
  const int count = static_cast<int>(run.size()) / stride;
  if (count != count_ || stride != stride_) {
    // Answers about another arrangement of values are not answers about this one.
    answered_.clear();
  }
  stride_ = stride;
  count_ = count;
  uploaded_ = run;
  if (count == 0) return;

  if (count > capacity_ || values_ == 0 || stride * capacity_ > allocated_) {
    // A question in flight is about the buffers that are going.
    if (fence_ != nullptr) glext::DropFence(fence_);
    fence_ = nullptr;
    if (values_ != 0) rlUnloadShaderBuffer(values_);
    if (answers_ != 0) rlUnloadShaderBuffer(answers_);
    capacity_ = std::max(16, count * 2);
    allocated_ = capacity_ * stride;
    values_ = rlLoadShaderBuffer(
        static_cast<unsigned int>(static_cast<std::size_t>(allocated_) * sizeof(float)),
        nullptr, RL_DYNAMIC_DRAW);
    // Four numbers per value, and room after them for one dragged value.
    answers_ = rlLoadShaderBuffer(
        static_cast<unsigned int>((static_cast<std::size_t>(capacity_) * vizglsl::kAnswerFloats +
                                   static_cast<std::size_t>(stride)) * sizeof(float)),
        nullptr, RL_DYNAMIC_READ);
  }
  rlUpdateShaderBuffer(values_, run.data(),
                       static_cast<unsigned int>(run.size() * sizeof(float)), 0);
}

bool VizValues::Ask(const VizGpu& viz, const VizFrame& frame, int dragOne) {
  if (fence_ != nullptr || count_ == 0 || !viz.Ready() || viz.Stride() != stride_) return false;
  viz.Ask(values_, answers_, count_, frame, dragOne);
  fence_ = glext::Fence();
  askedCount_ = count_;
  askedDrag_ = viz.Drags() ? dragOne : -1;
  dragFloats_ = viz.ValueFloats();
  return true;
}

bool VizValues::Collect() {
  if (fence_ == nullptr || !glext::Passed(fence_)) return false;
  glext::DropFence(fence_);
  fence_ = nullptr;
  const int floats = askedCount_ * vizglsl::kAnswerFloats;
  const int total = floats + (askedDrag_ >= 0 ? dragFloats_ : 0);
  std::vector<float> read(static_cast<std::size_t>(total), 0.0f);
  rlReadShaderBuffer(answers_, read.data(), static_cast<unsigned int>(total * sizeof(float)), 0);
  answered_.assign(read.begin(), read.begin() + floats);
  if (askedDrag_ >= 0) {
    dragged_.assign(read.begin() + floats, read.end());
    haveDrag_ = true;
  }
  return true;
}

bool VizValues::Position(int index, Vector3* out) const {
  const std::size_t at = static_cast<std::size_t>(index) * vizglsl::kAnswerFloats;
  if (index < 0 || at + 2 >= answered_.size()) return false;
  *out = {answered_[at], answered_[at + 1], answered_[at + 2]};
  return true;
}

float VizValues::Reach(int index) const {
  const std::size_t at = static_cast<std::size_t>(index) * vizglsl::kAnswerFloats + 3;
  if (index < 0 || at >= answered_.size()) return 1.0e9f;
  return answered_[at];
}

bool VizValues::TakeDrag(std::vector<float>* parts) {
  if (!haveDrag_) return false;
  haveDrag_ = false;
  *parts = dragged_;
  return true;
}

void VizValues::Release() {
  if (fence_ != nullptr) glext::DropFence(fence_);
  fence_ = nullptr;
  if (values_ != 0) rlUnloadShaderBuffer(values_);
  if (answers_ != 0) rlUnloadShaderBuffer(answers_);
  values_ = 0;
  answers_ = 0;
  capacity_ = 0;
  count_ = 0;
  stride_ = 0;
  uploaded_.clear();
  answered_.clear();
  haveDrag_ = false;
  allocated_ = 0;
}

// ------------------------------------------------------------------ VizPane

void VizPane::Add(const VizItem& item, Color tint, const float* parts, int count) {
  for (int k = 0; k < count; ++k) {
    if (!std::isfinite(parts[k])) return;
  }
  run_.push_back(static_cast<float>(tint.r) / 255.0f);
  run_.push_back(static_cast<float>(tint.g) / 255.0f);
  run_.push_back(static_cast<float>(tint.b) / 255.0f);
  run_.push_back(static_cast<float>(tint.a) / 255.0f);
  run_.insert(run_.end(), parts, parts + count);
  items_.push_back(item);
}

void VizPane::Settle(const VizGpu& viz, const VizFrame& frame, std::vector<float> about,
                     bool pointerOver, int dragOne) {
  drags_ = viz.Drags();
  values_.Upload(run_, viz.Stride());
  about.insert(about.end(), run_.begin(), run_.end());
  const std::vector<float> knobs = viz.Knobs();
  about.insert(about.end(), knobs.begin(), knobs.end());
  if (!pointerOver && dragOne < 0 && about == askedAbout_) return;
  if (values_.Ask(viz, frame, dragOne)) askedAbout_ = std::move(about);
}

void VizPane::Draw(const VizGpu& viz, const VizFrame& frame) const {
  viz.Draw(values_.Buffer(), values_.Count(), frame);
}

int VizPane::Find(const CodeBlock* block, const std::string& name) const {
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].sample < 0 && items_[i].block == block && items_[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int VizPane::Nearest(float reach, const std::function<bool(const VizItem&)>& accept) const {
  int   best = -1;
  float nearest = reach;
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (!accept(items_[i])) continue;
    const float away = values_.Reach(static_cast<int>(i));
    if (away >= nearest) continue;
    nearest = away;
    best = static_cast<int>(i);
  }
  return best;
}

// ----------------------------------------------------------------- VizStore

// One of the repo's vizes sketches, read and built but never put on a canvas.
struct VizStore::Library {
  std::string                             path;
  std::filesystem::file_time_type         stamp{};
  std::vector<std::unique_ptr<CodeBlock>> blocks;
  mathprogram::Canvas                     canvas;
  std::unordered_map<std::string, CodeBlock*> vizes;
  // A viz that failed says so once, in the log, rather than every frame.
  std::unordered_map<std::string, bool> told;
};

VizStore::VizStore() = default;
VizStore::~VizStore() = default;

void VizStore::Load(const std::string& path) {
  auto library = std::make_unique<Library>();
  library->path = path;
  std::error_code error;
  library->stamp = std::filesystem::last_write_time(path, error);

  // The canvas format, read only as far as the boxes' lines: `block ...`, then
  // one `> ` line per line of the box, then `end`.
  std::ifstream file(path);
  std::vector<std::vector<std::string>> boxes;
  bool inBlock = false;
  for (std::string line; std::getline(file, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("block ", 0) == 0) {
      boxes.emplace_back();
      inBlock = true;
    } else if (line == "end") {
      inBlock = false;
    } else if (inBlock && line.rfind(">", 0) == 0) {
      boxes.back().push_back(line.size() > 2 ? line.substr(2) : std::string());
    }
  }

  for (std::vector<std::string>& lines : boxes) {
    library->blocks.push_back(std::make_unique<CodeBlock>(Vector2{0.0f, 0.0f}, 20.0f,
                                                          std::move(lines)));
    library->blocks.back()->SetLibrary(true);
  }
  // Built once alone, so each box knows what it offers, and again against the
  // sketch's canvas -- which is where the algebra comes from.
  std::vector<const mathprogram::Program*> programs;
  for (const std::unique_ptr<CodeBlock>& block : library->blocks) {
    block->RequestCompile();
    programs.push_back(&block->Program());
  }
  library->canvas = mathprogram::Gather(programs);
  for (const std::unique_ptr<CodeBlock>& block : library->blocks) {
    block->SetCanvas(&library->canvas);
    block->RequestCompile();
    if (!block->VizType().empty()) library->vizes.emplace(block->VizType(), block.get());
  }
  libraries_.push_back(std::move(library));
}

void VizStore::Sync(const std::vector<std::unique_ptr<CodeBlock>>& blocks,
                    const clifford::Signature& signature) {
  local_.clear();
  samplers_.clear();
  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    if (!block->SamplerType().empty()) samplers_[block->SamplerType()] = block.get();
    if (!block->VizType().empty()) local_[block->VizType()] = block.get();
  }

  // The built-in vizes: those of every canvas, and those of this canvas's
  // algebra. Read again when the algebra changes, or when a file has been saved
  // since -- which is what makes editing one take effect everywhere. Looked at
  // once a second rather than every frame.
  const double now = GetTime();
  bool reload = !loaded_ || signature.p != signature_.p || signature.q != signature_.q ||
                signature.r != signature_.r;
  if (!reload && now - checkedAt_ > 1.0) {
    checkedAt_ = now;
    for (const std::unique_ptr<Library>& library : libraries_) {
      std::error_code error;
      if (std::filesystem::last_write_time(library->path, error) != library->stamp) reload = true;
    }
  }
  if (reload) Reload(signature);
}

void VizStore::Reload(const clifford::Signature& signature) {
  ReleaseGpu();
  libraries_.clear();
  loaded_ = true;
  signature_ = signature;
  checkedAt_ = GetTime();
  const std::string dir = VizesFolder();
  const std::string algebra = "Cl(" + std::to_string(signature.p) + "," +
                              std::to_string(signature.q) + "," + std::to_string(signature.r) +
                              ") vizes.sketch";
  for (const std::string& name : {std::string("vizes.sketch"), algebra}) {
    const std::string path = dir + "/" + name;
    std::error_code error;
    if (std::filesystem::exists(path, error)) Load(path);
  }
}

const VizGpu* VizStore::Find(const std::string& type, const std::string& dw) {
  const std::string key = type + "@" + dw;
  // The canvas's own, if it has one that works. One being written, or wrong,
  // gives way to the built-in rather than leaving nothing drawn.
  if (const auto found = local_.find(key); found != local_.end()) {
    found->second->RefreshViz();
    if (const VizGpu* viz = found->second->VizProgram(); viz != nullptr) return viz;
  }
  for (const std::unique_ptr<Library>& library : libraries_) {
    const auto found = library->vizes.find(key);
    if (found == library->vizes.end()) continue;
    CodeBlock* block = found->second;
    // Compiled the first time it is wanted.
    if (block->Program().state() == mathprogram::State::Shading) block->Poll();
    // Its knobs are its own sketch's, as they were saved.
    block->PullImports(library->canvas);
    block->RefreshViz();
    if (const VizGpu* viz = block->VizProgram(); viz != nullptr) return viz;
    if (block->Program().state() == mathprogram::State::Failed && !library->told[key]) {
      library->told[key] = true;
      TraceLog(LOG_WARNING, "VIZ: %s, line %d: %s", library->path.c_str(),
               block->Program().error().line, block->Program().error().message.c_str());
    }
  }
  return nullptr;
}

bool VizStore::Example(const std::vector<std::unique_ptr<CodeBlock>>& blocks,
                       const clifford::Signature& signature, const std::string& type,
                       const std::string& name, std::vector<float>* out) {
  // Asked as a canvas is opened, before anything has said which built-ins
  // apply: the box asking knows its algebra, so they are read now.
  if (!loaded_ || signature.p != signature_.p || signature.q != signature_.q ||
      signature.r != signature_.r) {
    Reload(signature);
  }
  const std::string prefix = type + "@";
  const unsigned int seed = types::NameSeed(name);
  const auto ask = [&](const std::string& key, CodeBlock* block) {
    if (!block->Program().viz().example) return false;
    if (block->Program().state() == mathprogram::State::Shading) block->Poll();
    const VizGpu* viz = block->VizProgram();
    if (viz == nullptr || !viz->HasExample()) return false;
    const VizFrame frame =
        exampleFrames_ ? exampleFrames_(key.substr(prefix.size())) : VizFrame{};
    return viz->Example(seed, frame, out);
  };
  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    const std::string& key = block->VizType();
    if (key.rfind(prefix, 0) == 0 && ask(key, block.get())) return true;
  }
  for (const std::unique_ptr<Library>& library : libraries_) {
    for (const auto& [key, block] : library->vizes) {
      if (key.rfind(prefix, 0) == 0 && ask(key, block)) return true;
    }
  }
  return false;
}

bool VizStore::Has(const std::string& type, const std::string& dw) const {
  const std::string key = type + "@" + dw;
  if (local_.count(key) > 0) return true;
  for (const std::unique_ptr<Library>& library : libraries_) {
    if (library->vizes.count(key) > 0) return true;
  }
  return false;
}

std::map<std::string, mathprogram::FunctionViz> VizStore::FunctionVizes(
    const std::vector<std::unique_ptr<CodeBlock>>& blocks,
    std::unordered_map<std::string, mathprogram::Literal>* knobs) {
  // Only the ones some box on the canvas has a function to draw with.
  std::vector<std::string> wanted;
  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    for (const mathprogram::Function& fn : block->Program().functions()) {
      const std::string type = vizglsl::FunctionTypeOf(mathline::GlslTypeName(fn.inputType),
                                                       mathline::GlslTypeName(fn.outputType));
      for (const std::string& dw : vizglsl::FunctionWindows(type)) {
        const std::string key = type + "@" + dw;
        if (std::find(wanted.begin(), wanted.end(), key) == wanted.end()) wanted.push_back(key);
      }
    }
  }
  std::map<std::string, mathprogram::FunctionViz> out;
  // Its text, and the knobs it reads: its own, and what it took from its
  // sketch, whose numbers it has just fetched.
  const auto offer = [&](const std::string& key, const CodeBlock& block) {
    const mathprogram::Program& program = block.Program();
    mathprogram::FunctionViz    viz;
    viz.text = program.viz().spliced;
    viz.grab = program.viz().grab;
    viz.example = program.viz().example;
    const auto take = [&](const mathprogram::Literal& literal) {
      viz.knobs.push_back(literal);
      (*knobs)[literal.uniform] = literal;
    };
    for (const mathprogram::Variable& variable : program.variables()) {
      for (const mathprogram::Literal& literal : variable.literals) take(literal);
    }
    for (const mathprogram::Literal& literal : program.imported()) take(literal);
    out[key] = std::move(viz);
  };
  for (const std::string& key : wanted) {
    if (const auto found = local_.find(key);
        found != local_.end() && found->second->FunctionVizReady()) {
      offer(key, *found->second);
      continue;
    }
    for (const std::unique_ptr<Library>& library : libraries_) {
      const auto found = library->vizes.find(key);
      if (found == library->vizes.end()) continue;
      CodeBlock* block = found->second;
      if (block->Program().state() == mathprogram::State::Shading) block->Poll();
      block->PullImports(library->canvas);
      if (block->FunctionVizReady()) {
        offer(key, *block);
        break;
      }
      if (block->Program().state() == mathprogram::State::Failed && !library->told[key]) {
        library->told[key] = true;
        TraceLog(LOG_WARNING, "VIZ: %s, line %d: %s", library->path.c_str(),
                 block->Program().error().line, block->Program().error().message.c_str());
      }
    }
  }
  return out;
}

const CodeBlock* VizStore::Sampler(const std::string& type) const {
  const auto found = samplers_.find(type);
  return (found == samplers_.end()) ? nullptr : found->second;
}

void VizStore::Clear() {
  local_.clear();
  samplers_.clear();
}

void VizStore::ReleaseGpu() {
  for (const std::unique_ptr<Library>& library : libraries_) {
    for (const std::unique_ptr<CodeBlock>& block : library->blocks) block->ReleaseGpu();
  }
  if (g_emptyVao != 0) rlUnloadVertexArray(g_emptyVao);
  g_emptyVao = 0;
}

}  // namespace sb
