#include "viz.h"

#include "blocks.h"
#include "rlgl.h"

namespace sb {

void Viz::Draw(const std::vector<float>& tris, const std::vector<float>& segments, float value,
               Rectangle area, Vector4 view, float scale, Color tint) const {
  if (!haveShader || (tris.size() < 6 && segments.size() < 4)) return;

  BeginShaderMode(shader);
  if (valueLoc >= 0) SetShaderValue(shader, valueLoc, &value, SHADER_UNIFORM_FLOAT);
  if (areaLoc >= 0) {
    const float values[4] = {area.x, area.y, area.width, area.height};
    SetShaderValue(shader, areaLoc, values, SHADER_UNIFORM_VEC4);
  }
  if (viewLoc >= 0) {
    const float values[4] = {view.x, view.y, view.z, view.w};
    SetShaderValue(shader, viewLoc, values, SHADER_UNIFORM_VEC4);
  }
  if (scaleLoc >= 0) SetShaderValue(shader, scaleLoc, &scale, SHADER_UNIFORM_FLOAT);

  // A flat shape has no back face, so which way round its triangles are wound
  // must not decide whether you can see it. raylib culls back faces by default,
  // and screen space turns the y axis upside down, so a ring written the way
  // anyone would write one -- anticlockwise, in the coordinates it was thought
  // in -- comes out clockwise here and vanishes. Nobody authoring a viz should
  // have to know that, so culling is simply off while one is drawn. Off around
  // the *flush*, which is what EndShaderMode below does, rather than around the
  // submission: the batch is deferred.
  rlDisableBackfaceCulling();

  // Straight into raylib's batch, exactly as DrawTriangle does, so the mesh
  // goes through the same path everything else on the canvas does and the
  // shader's `mvp` is filled in for free. The vertices submitted are the viz's
  // own units; putting them somewhere is the shader's whole job.
  const std::size_t triangles = tris.size() / 6;
  if (triangles > 0) {
    rlBegin(RL_TRIANGLES);
    rlColor4ub(tint.r, tint.g, tint.b, tint.a);
    for (std::size_t t = 0; t < triangles; ++t) {
      const std::size_t at = t * 6;
      rlVertex2f(tris[at + 0], tris[at + 1]);
      rlVertex2f(tris[at + 2], tris[at + 3]);
      rlVertex2f(tris[at + 4], tris[at + 5]);
    }
    rlEnd();
  }

  // A path is drawn as lines rather than as a ribbon of triangles: its width has
  // to be constant on screen, and the mesh is in value units, which the placing
  // shader is free to stretch by different amounts along each axis.
  const std::size_t segs = segments.size() / 4;
  if (segs > 0) {
    rlBegin(RL_LINES);
    rlColor4ub(tint.r, tint.g, tint.b, tint.a);
    for (std::size_t s = 0; s < segs; ++s) {
      const std::size_t at = s * 4;
      rlVertex2f(segments[at + 0], segments[at + 1]);
      rlVertex2f(segments[at + 2], segments[at + 3]);
    }
    rlEnd();
  }

  EndShaderMode();
  rlEnableBackfaceCulling();
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
    // that half and leaves the other alone.
    if (!block->VizMesh().empty()) viz.mesh = &block->VizMesh();
    if (!block->VizLines().empty()) viz.lines = &block->VizLines();
    // The C++ half's lines, for injecting into programs that have a value of
    // this type. Taken whether or not it compiled standalone: a box is the mesh
    // half if C++ would have it, and the sigil line is not part of the body.
    if (block->BuildsAsCxx()) {
      std::vector<std::string>& body = sources[type];
      if (body.empty()) {
        for (const std::string& raw : block->Lines()) {
          if (mathline::Parse(raw).kind == mathline::Kind::Viz) continue;
          body.push_back(raw);
        }
      }
    }
    if (block->HasVizShader()) {
      viz.shader = block->VizShader();
      viz.haveShader = true;
      viz.valueLoc = block->VizUniform("value");
      viz.areaLoc = block->VizUniform("sbArea");
      viz.viewLoc = block->VizUniform("sbView");
      viz.scaleLoc = block->VizUniform("sbScale");
    }
  }

  const bool changed = sources != sources_;
  sources_ = std::move(sources);
  return changed;
}

const Viz* VizStore::Find(const std::string& type) const {
  const auto found = vizes_.find(type);
  return (found == vizes_.end()) ? nullptr : &found->second;
}

}  // namespace sb
