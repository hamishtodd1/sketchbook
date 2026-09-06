#include "viz.h"

#include "glext.h"
#include "raymath.h"

#include "blocks.h"
#include "rlgl.h"

namespace sb {

// One empty vertex array, shared by every viz. A core-profile draw needs some
// vertex array bound even when it reads nothing from one, and this is that
// nothing: the geometry arrives through the storage buffer instead.
namespace {
unsigned int g_emptyVao = 0;
}  // namespace

void VizStore::ReleaseGpu() {
  if (g_emptyVao == 0) return;
  rlUnloadVertexArray(g_emptyVao);
  g_emptyVao = 0;
}

void Viz::Draw(float value, Rectangle area, Vector4 view, float scale, Color tint) const {
  if (!haveShader || meshBlock == nullptr) return;

  // The geometry for *this* value, laid down by the viz's own compute shader.
  // It only actually runs when the value has moved.
  meshBlock->RunVizMesh(value);
  const int tris = meshBlock->VizTriCount();
  const int segs = meshBlock->VizLineCount();
  if (tris < 3 && segs < 2) return;

  // Whatever raylib had queued goes first: this draw goes round its batch.
  rlDrawRenderBatchActive();

  rlEnableShader(shader.id);

  // Everything on this canvas is drawn in screen space, so the shader's job
  // ends in pixels and this is what turns pixels into clip space. raylib keeps
  // the two halves of it and multiplies them the same way for its own batch.
  const Matrix mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
  rlSetUniformMatrix(shader.locs[SHADER_LOC_MATRIX_MVP], mvp);

  if (valueLoc >= 0) rlSetUniform(valueLoc, &value, RL_SHADER_UNIFORM_FLOAT, 1);
  if (areaLoc >= 0) {
    const float values[4] = {area.x, area.y, area.width, area.height};
    rlSetUniform(areaLoc, values, RL_SHADER_UNIFORM_VEC4, 1);
  }
  if (viewLoc >= 0) {
    const float values[4] = {view.x, view.y, view.z, view.w};
    rlSetUniform(viewLoc, values, RL_SHADER_UNIFORM_VEC4, 1);
  }
  if (scaleLoc >= 0) rlSetUniform(scaleLoc, &scale, RL_SHADER_UNIFORM_FLOAT, 1);
  if (tintLoc >= 0) {
    const float values[4] = {static_cast<float>(tint.r) / 255.0f,
                             static_cast<float>(tint.g) / 255.0f,
                             static_cast<float>(tint.b) / 255.0f,
                             static_cast<float>(tint.a) / 255.0f};
    rlSetUniform(tintLoc, values, RL_SHADER_UNIFORM_VEC4, 1);
  }

  // A flat shape has no back face, so which way round its triangles are wound
  // must not decide whether you can see it. raylib culls back faces by default,
  // and screen space turns the y axis upside down, so a ring written the way
  // anyone would write one -- anticlockwise, in the coordinates it was thought
  // in -- comes out clockwise here and vanishes. Nobody authoring a viz should
  // have to know that, so culling is simply off while one is drawn.
  rlDisableBackfaceCulling();
  if (g_emptyVao == 0) g_emptyVao = rlLoadVertexArray();
  rlEnableVertexArray(g_emptyVao);

  if (tris >= 3) {
    rlBindShaderBuffer(meshBlock->VizTriBuffer(), 1);
    glext::DrawArrays(glext::kTriangles, 0, tris);
  }
  if (segs >= 2) {
    rlBindShaderBuffer(meshBlock->VizLineBuffer(), 1);
    glext::DrawArrays(glext::kLines, 0, segs);
  }

  rlDisableVertexArray();
  rlEnableBackfaceCulling();
  rlDisableShader();
}

bool VizStore::Sync(const std::vector<std::unique_ptr<CodeBlock>>& blocks) {
  vizes_.clear();
  mathprogram::VizSources sources;
  for (const std::unique_ptr<CodeBlock>& block : blocks) {
    const std::string& type = block->VizType();
    if (type.empty()) continue;

    Viz& viz = vizes_[type];
    // Two boxes, either order, and a box that has not built yet simply has not
    // said anything yet. Whichever half a box turned out to be, it fills in
    // that half and leaves the other alone. Nothing is injected into anybody
    // else's program any more: the mesh half is a compute shader of its own.
    if (block->HasVizMesh()) viz.meshBlock = block.get();
    if (block->HasVizShader()) {
      viz.shader = block->VizShader();
      viz.haveShader = true;
      viz.valueLoc = block->VizUniform("value");
      viz.areaLoc = block->VizUniform("sbArea");
      viz.viewLoc = block->VizUniform("sbView");
      viz.scaleLoc = block->VizUniform("sbScale");
      viz.tintLoc = block->VizUniform("sbTint");
    }
  }

  const bool changed = sources != sources_;
  sources_ = std::move(sources);
  return changed;
}

const Viz* VizStore::Find(const std::string& type, const std::string& window) const {
  const auto found = vizes_.find(window.empty() ? type : (type + "@" + window));
  return (found == vizes_.end()) ? nullptr : &found->second;
}

}  // namespace sb
