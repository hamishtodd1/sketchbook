// Claude's tools on the canvas: what a conversation's reply asks for, carried
// out on the thread that owns the canvas and answered.
//
// Every change a tool makes goes through the same doors a person's would --
// boxes placed by PlaceBlock, removed by RemoveBlock, values set as a knob sets
// them -- and is its own undo step. A box is spoken of by its id, `b7`.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "app.h"
#include "screen.h"
#include "hover.h"
#include "rlgl.h"

#ifndef SKETCHBOOK_VIZES_DIR
#define SKETCHBOOK_VIZES_DIR "vizes"
#endif

namespace sb {
namespace {

json::Value TextContent(const std::string& text) {
  json::Value block = json::Value::Object();
  block.Set("type", "text");
  block.Set("text", text);
  json::Value content = json::Value::Array();
  content.Push(std::move(block));
  return content;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t              from = 0;
  while (true) {
    const std::size_t nl = text.find('\n', from);
    std::string line = text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (nl == std::string::npos) break;
    from = nl + 1;
  }
  return lines;
}

std::string JoinLines(const std::vector<std::string>& lines) {
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) out += '\n';
    out += lines[i];
  }
  return out;
}

std::string Number(float value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.4g", static_cast<double>(value));
  return buffer;
}

// A box's lines as they appear in a canvas file, read straight off the disk:
// `block ...`, then `> ` lines, then `end`.
std::string BoxesOfFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  std::string out;
  std::string line;
  bool        inBlock = false;
  int         count = 0;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("block", 0) == 0) {
      inBlock = true;
      out += "--- box " + std::to_string(++count) + "\n";
    } else if (line == "end") {
      inBlock = false;
    } else if (inBlock && line.rfind(">", 0) == 0) {
      out += line.size() > 2 ? line.substr(2) : std::string();
      out += '\n';
    }
  }
  return out;
}

}  // namespace

// ------------------------------------------------------------------ reading

CodeBlock* App::BoxNamed(const std::string& id) {
  std::string digits = id;
  if (!digits.empty() && (digits[0] == 'b' || digits[0] == 'B')) digits.erase(0, 1);
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
    return nullptr;
  }
  const int wanted = std::atoi(digits.c_str());
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (block->Id() == wanted) return block.get();
  }
  return nullptr;
}

std::string App::DescribeBox(CodeBlock& block) {
  const mathprogram::Program& program = block.Program();
  std::string out = "b" + std::to_string(block.Id());
  if (block.IsText()) out += " (text box)";
  else if (!program.vizType().empty()) out += " (viz box: " + program.vizType() + ")";
  else out += " (code box)";
  out += "\n";

  const std::vector<std::string>& lines = block.Lines();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    char number[16];
    std::snprintf(number, sizeof(number), "%3zu| ", i + 1);
    out += number + lines[i] + "\n";
  }
  if (block.IsText()) return out;

  switch (program.state()) {
    case mathprogram::State::Empty: out += "Nothing to compile.\n"; break;
    case mathprogram::State::Shading: out += "Compiling.\n"; break;
    case mathprogram::State::ShaderReady: out += "Compiled.\n"; break;
    case mathprogram::State::Failed: {
      const compiler::Diagnostic& error = program.error();
      out += "Did not compile. Line " + std::to_string(error.line) + ": " + error.message + "\n";
      if (!error.secondary.empty()) out += "Then: " + error.secondary + "\n";
      break;
    }
  }

  if (!program.variables().empty()) {
    out += "Values:\n";
    for (const mathprogram::Variable& variable : program.variables()) {
      out += "  " + variable.name + " : " + variable.type;
      out += variable.free ? " (free: has a knob)" : " (derived)";
      if (variable.local) out += " (inside a function)";
      if (!variable.known) {
        out += " = not worked out yet";
      } else if (variable.kind == mathprogram::Kind::Real) {
        out += " = " + Number(variable.value);
      } else if (!variable.components.empty()) {
        // Each part by name -- `e012 = 0.5` -- since a list of bare numbers says
        // nothing about which blade is which.
        std::vector<std::string> names;
        if (variable.kind == mathprogram::Kind::Color) names = {"r", "g", "b"};
        else if (variable.kind == mathprogram::Kind::Multivector) {
          names = clifford::MemberNames(variable.type, block.Algebra());
        }
        if (names.size() != variable.components.size()) names.clear();
        out += " = (";
        for (std::size_t i = 0; i < variable.components.size(); ++i) {
          if (i > 0) out += ", ";
          if (!names.empty()) out += names[i] + " ";
          out += Number(variable.components[i]);
        }
        out += ")";
      }
      out += "\n";
    }
  }
  if (!program.functions().empty()) {
    out += "Functions:\n";
    for (const mathprogram::Function& fn : program.functions()) {
      out += "  " + fn.name + " : " + fn.inputType + " -> " + fn.outputType + " (lines " +
             std::to_string(fn.firstLine + 1) + "-" + std::to_string(fn.lastLine + 1) + ")\n";
    }
  }
  return out;
}

claude::CanvasView App::ViewCanvas() {
  claude::CanvasView view;
  const clifford::Signature& sig = canvas_.signature;
  view.header = "Algebra: Cl(" + std::to_string(sig.p) + "," + std::to_string(sig.q) + "," +
                std::to_string(sig.r) + ").";
  std::string selected;
  for (const std::unique_ptr<CodeBlock>& block : codeBlocks_) {
    if (block->Empty()) continue;
    view.boxes.emplace_back(block->Id(), DescribeBox(*block));
    if (block->Selected()) selected += (selected.empty() ? "b" : ", b") + std::to_string(block->Id());
  }
  if (focus_ != nullptr && !focus_->Empty()) {
    view.status += "Hamish's caret is in b" + std::to_string(focus_->Id()) + ", on line " +
                   std::to_string(focus_->CaretLine() + 1) + ".\n";
  }
  if (!selected.empty()) view.status += "Selected: " + selected + ".\n";
  return view;
}

std::string App::DescribeCanvas() {
  const claude::CanvasView view = ViewCanvas();
  std::string out = "The canvas as it is now. " + view.header + "\n";
  for (const auto& [id, description] : view.boxes) out += "\n" + description;
  if (view.boxes.empty()) out += "\nThere are no boxes on it.\n";
  if (!view.status.empty()) out += "\n" + view.status;
  return out;
}

bool App::DwUnder(Vector2 point, Rectangle* world, std::string* kind) {
  // Nothing under a conversation is being pointed at.
  if (chats_.ItemAt(point, viewport_) >= 0) return false;
  // The ones drawn last are on top, so they are asked first.
  struct Candidate {
    const char*            name;
    std::vector<Rectangle> rects;
  };
  std::vector<Candidate> candidates(7);
  candidates[0].name = "space";
  spaces_.Rects(viewport_, fonts_, &candidates[0].rects);
  candidates[1].name = "volume";
  volumes_.Rects(viewport_, fonts_, &candidates[1].rects);
  candidates[2].name = "surface";
  surfaces_.Rects(viewport_, fonts_, &candidates[2].rects);
  candidates[3].name = "colour";
  colours_.Rects(viewport_, fonts_, &candidates[3].rects);
  candidates[4].name = "picture";
  shaders_.Rects(viewport_, fonts_, &candidates[4].rects);
  candidates[5].name = "plot";
  plots_.Rects(viewport_, fonts_, &candidates[5].rects);
  candidates[6].name = "number line";
  axes_.Rects(viewport_, fonts_, &candidates[6].rects);
  for (const Candidate& candidate : candidates) {
    for (auto it = candidate.rects.rbegin(); it != candidate.rects.rend(); ++it) {
      if (!CheckCollisionPointRec(point, *it)) continue;
      const Vector2 a = viewport_.ToWorld({it->x, it->y});
      const Vector2 b = viewport_.ToWorld({it->x + it->width, it->y + it->height});
      *world = {a.x, a.y, b.x - a.x, b.y - a.y};
      *kind = candidate.name;
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------ changing

void App::BeforeToolChange() {
  // Whatever Hamish did first is its own step; anything that merely happened
  // meanwhile -- a build coming back -- is folded into where things are.
  CommitUndo();
  undoBase_ = Snapshot();
}

void App::AfterToolChange() {
  undoPending_ = true;
  undoLastInput_ = GetTime();
}

bool App::Settled(const ToolJob& job) const {
  const CodeBlock* block = job.waitFor;
  if (block == nullptr) return true;
  if (job.frames < 3) return false;
  const mathprogram::State state = block->Program().state();
  if (state == mathprogram::State::Shading || block->Dirty()) return job.frames > 600;
  // Compiled: its values come back a frame or two later.
  if (state == mathprogram::State::ShaderReady && job.frames < 90) {
    for (const mathprogram::Variable& variable : block->Program().variables()) {
      if (!variable.local && !variable.known) return false;
    }
  }
  return true;
}

bool App::StartTool(ToolJob& job) {
  claude::Conversation& conversation = *job.window->conversation;
  const std::string&    name = job.call.name;
  const json::Value&    input = job.call.input;
  TraceLog(LOG_INFO, "CLAUDE: %s %s", name.c_str(), json::Write(input).substr(0, 400).c_str());
  const auto answer = [&](const std::string& text, bool error) {
    conversation.Answer(job.call.id, TextContent(text), error);
    return true;
  };

  if (name == "read_canvas") {
    conversation.SawAll(ViewCanvas());
    return answer(DescribeCanvas(), false);
  }

  // **A picture only of what Hamish pointed at**: the display window his pointer
  // was on when he sent his message, and nothing else.
  if (name == "look") {
    if (!job.window->lookValid) {
      return answer("His pointer was not on a display window when he sent his message, so there "
                    "is nothing to look at. Ask him to point at the window and say so.",
                    true);
    }
    job.look = true;
    job.lookWorld = job.window->lookWorld;
    job.lookKind = job.window->lookKind;
    return false;
  }

  if (name == "read_builtin_vizes") {
    const std::filesystem::path dir = VizesFolder();
    const clifford::Signature&  sig = canvas_.signature;
    std::string out = "Built-in vizes for types of no algebra (vizes.sketch):\n" +
                      BoxesOfFile(dir / "vizes.sketch");
    const std::string algebra = "Cl(" + std::to_string(sig.p) + "," + std::to_string(sig.q) +
                                "," + std::to_string(sig.r) + ") vizes.sketch";
    const std::string theirs = BoxesOfFile(dir / algebra);
    if (!theirs.empty()) out += "\nBuilt-in vizes for this canvas's algebra (" + algebra + "):\n" + theirs;
    return answer(out, false);
  }

  if (name == "make_box") {
    const std::string& text = input["text"].String();
    if (text.empty()) return answer("A box needs some text.", true);
    BeforeToolChange();
    // Beside the conversation, each one under the last.
    ChatWindow&   window = *job.window;
    const Vector2 desired = {window.worldPos.x + window.width + 40.0f,
                             window.worldPos.y + window.madeBelow};
    codeBlocks_.push_back(std::make_unique<CodeBlock>(desired, kDefaultBlockEm, SplitLines(text)));
    CodeBlock* block = codeBlocks_.back().get();
    PlaceBlock(block, desired);
    block->RequestCompile();
    const Rectangle bounds = block->WorldBounds(viewport_, fonts_);
    window.madeBelow = std::max(window.madeBelow, bounds.y + bounds.height + 40.0f - window.worldPos.y);
    AfterToolChange();
    job.waitFor = block;
    job.said = "Made b" + std::to_string(block->Id()) + ", beside the conversation.\n";
    return false;
  }

  // Everything else is about one box.
  CodeBlock* block = BoxNamed(input["box"].String());
  if (block == nullptr) {
    return answer("There is no box " + input["box"].String() +
                      " -- it may have been deleted or rebuilt. Read the canvas again.",
                  true);
  }
  if (block == focus_) {
    return answer("Hamish is typing in b" + std::to_string(block->Id()) +
                      " right now, so it was left alone.",
                  true);
  }

  if (name == "edit_box" || name == "replace_box") {
    std::string text = JoinLines(block->Lines());
    if (name == "edit_box") {
      const std::string& from = input["old"].String();
      const std::string& to = input["new"].String();
      if (from.empty()) return answer("`old` is empty.", true);
      std::size_t count = 0;
      for (std::size_t at = text.find(from); at != std::string::npos;
           at = text.find(from, at + from.size())) {
        ++count;
      }
      if (count != 1) {
        return answer(count == 0 ? "`old` is not in the box. Its text is:\n" + text
                                 : "`old` occurs " + std::to_string(count) +
                                       " times; make it longer so it picks out one.",
                      true);
      }
      text.replace(text.find(from), from.size(), to);
    } else {
      text = input["text"].String();
    }
    BeforeToolChange();
    block->RestoreLines(SplitLines(text));
    AfterToolChange();
    job.waitFor = block;
    job.said = "Changed b" + std::to_string(block->Id()) + ".\n";
    return false;
  }

  if (name == "delete_box") {
    const int id = block->Id();
    BeforeToolChange();
    RemoveBlock(block);
    AfterToolChange();
    return answer("Deleted b" + std::to_string(id) + ".", false);
  }

  if (name == "set_value") {
    const std::string&           variableName = input["name"].String();
    const mathprogram::Variable* variable = block->FindVariable(variableName);
    if (variable == nullptr) return answer("b" + std::to_string(block->Id()) + " has no " + variableName + ".", true);
    if (!variable->free) {
      return answer(variableName + " is derived: move what it is made of instead.", true);
    }
    const json::Value& value = input["value"];
    BeforeToolChange();
    if (variable->kind == mathprogram::Kind::Real && value.IsNumber()) {
      block->SetVariableValue(variableName, static_cast<float>(value.Number()));
    } else if (value.IsArray()) {
      std::vector<float> parts;
      for (const json::Value& part : value.Items()) parts.push_back(static_cast<float>(part.Number()));
      if (variable->kind == mathprogram::Kind::Color && parts.size() == 3) {
        block->SetColorValue(variableName, parts.data());
      } else if (variable->kind == mathprogram::Kind::Multivector &&
                 parts.size() == variable->components.size()) {
        block->SetComponentValues(variableName, parts);
      } else {
        return answer("That is not the right shape of value for " + variableName + ".", true);
      }
    } else {
      return answer("That is not the right shape of value for " + variableName + ".", true);
    }
    AfterToolChange();
    job.waitFor = block;
    job.said = "Set " + variableName + ".\n";
    return false;
  }

  return answer("There is no tool called " + name + ".", true);
}

void App::ServeTools() {
  // New calls, from every conversation.
  for (const std::unique_ptr<ChatWindow>& window : chats_.Windows()) {
    for (claude::ToolCall& call : window->conversation->TakeToolCalls()) {
      ToolJob job;
      job.window = window.get();
      job.call = std::move(call);
      if (!StartTool(job)) toolJobs_.push_back(std::move(job));
    }
  }

  // The ones waiting on a box.
  for (std::size_t i = 0; i < toolJobs_.size();) {
    ToolJob& job = toolJobs_[i];
    // A conversation thrown away takes its waiting tools with it, and a box
    // thrown away cannot be reported on.
    const bool windowAlive =
        std::any_of(chats_.Windows().begin(), chats_.Windows().end(),
                    [&](const std::unique_ptr<ChatWindow>& w) { return w.get() == job.window; });
    if (!windowAlive) {
      toolJobs_.erase(toolJobs_.begin() + static_cast<long>(i));
      continue;
    }
    const bool boxAlive =
        job.waitFor == nullptr ||
        std::any_of(codeBlocks_.begin(), codeBlocks_.end(),
                    [&](const std::unique_ptr<CodeBlock>& b) { return b.get() == job.waitFor; });
    if (!boxAlive) {
      job.window->conversation->Answer(job.call.id, TextContent("The box went away first."), true);
      toolJobs_.erase(toolJobs_.begin() + static_cast<long>(i));
      continue;
    }
    ++job.frames;
    if (job.look || !Settled(job)) {
      ++i;
      continue;
    }
    const std::string report = DescribeBox(*job.waitFor);
    job.window->conversation->Saw(job.waitFor->Id(), report);
    job.window->conversation->Answer(job.call.id, TextContent(job.said + report), false);
    toolJobs_.erase(toolJobs_.begin() + static_cast<long>(i));
  }
}

// ------------------------------------------------------------------ looking

void App::TakeLooks() {
  for (std::size_t i = 0; i < toolJobs_.size(); ++i) {
    ToolJob& job = toolJobs_[i];
    if (!job.look || !Settled(job)) continue;
    // A camera in the middle of a glide is left to finish; the look waits.
    if (viewport_.Gliding()) return;

    // What to frame: the window that was pointed at, in world units.
    Rectangle       world = job.lookWorld;
    constexpr float kMargin = 24.0f;
    world = {world.x - kMargin, world.y - kMargin, world.width + 2.0f * kMargin,
             world.height + 2.0f * kMargin};

    // Drawn once, unseen, with the camera on it -- then the real frame is drawn
    // over it, so nothing on screen moves.
    const float   screenW = static_cast<float>(ScreenWidth());
    const float   screenH = static_cast<float>(ScreenHeight());
    const Vector2 centre = viewport_.Centre();
    const float   zoom = viewport_.Zoom();
    const float   fit = std::clamp(std::min(screenW / world.width, screenH / world.height), 0.1f, 3.0f);
    viewport_.Reset({world.x + world.width * 0.5f, world.y + world.height * 0.5f}, fit);
    const bool held = hover::Held();
    hover::SetHeld(true);
    DrawCanvas(/*look=*/true);
    rlDrawRenderBatchActive();
    const int      renderW = GetRenderWidth();
    const int      renderH = GetRenderHeight();
    unsigned char* pixels = rlReadScreenPixels(renderW, renderH);
    const Vector2  topLeft = viewport_.ToScreen({world.x, world.y});
    const Vector2  bottomRight = viewport_.ToScreen({world.x + world.width, world.y + world.height});
    hover::SetHeld(held);
    viewport_.Reset(centre, zoom);

    json::Value content = json::Value::Array();
    if (pixels != nullptr) {
      Image image{};
      image.data = pixels;
      image.width = renderW;
      image.height = renderH;
      image.mipmaps = 1;
      image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
      // In whole pixels. raylib's ImageCrop sizes its buffer from the width times
      // the height but copies rows from the floor of the top to the floor of the
      // bottom, so a rectangle with a fractional edge copies a row more than it
      // made room for -- a write off the end of the buffer, and a crash that
      // comes and goes with whatever memory follows it.
      const float sx = static_cast<float>(renderW) / screenW;
      const float sy = static_cast<float>(renderH) / screenH;
      const int   x0 = std::clamp(static_cast<int>(std::floor(topLeft.x * sx)), 0, renderW);
      const int   y0 = std::clamp(static_cast<int>(std::floor(topLeft.y * sy)), 0, renderH);
      const int   x1 = std::clamp(static_cast<int>(std::ceil(bottomRight.x * sx)), 0, renderW);
      const int   y1 = std::clamp(static_cast<int>(std::ceil(bottomRight.y * sy)), 0, renderH);
      if (x1 - x0 > 8 && y1 - y0 > 8) {
        ImageCrop(&image, {static_cast<float>(x0), static_cast<float>(y0),
                           static_cast<float>(x1 - x0), static_cast<float>(y1 - y0)});
      }
      // Big enough to read, small enough not to cost much to send.
      constexpr int kMaxSide = 1200;
      if (image.width > kMaxSide || image.height > kMaxSide) {
        const float scale = static_cast<float>(kMaxSide) / static_cast<float>(std::max(image.width, image.height));
        ImageResize(&image, static_cast<int>(image.width * scale), static_cast<int>(image.height * scale));
      }
      int            size = 0;
      unsigned char* png = ExportImageToMemory(image, ".png", &size);
      UnloadImage(image);
      if (png != nullptr && size > 0) {
        json::Value source = json::Value::Object();
        source.Set("type", "base64");
        source.Set("media_type", "image/png");
        source.Set("data", claude::Base64(png, static_cast<std::size_t>(size)));
        json::Value picture = json::Value::Object();
        picture.Set("type", "image");
        picture.Set("source", std::move(source));
        content.Push(std::move(picture));
      }
      if (png != nullptr) MemFree(png);
    }
    json::Value words = json::Value::Object();
    words.Set("type", "text");
    words.Set("text", content.Size() > 0 ? "The " + job.lookKind + " window, as drawn now."
                                         : std::string("Could not read the picture back."));
    content.Push(std::move(words));
    job.window->conversation->Answer(job.call.id, std::move(content), false);
    toolJobs_.erase(toolJobs_.begin() + static_cast<long>(i));
    // One a frame: each costs a whole extra drawing of the canvas.
    return;
  }
}

}  // namespace sb
