#include "prompt.h"

#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#ifndef SKETCHBOOK_PROMPT_FILE
#define SKETCHBOOK_PROMPT_FILE "prompt/system.md"
#endif

namespace sb::prompt {
namespace {

// Only if the file cannot be read: the prompt proper is `prompt/system.md`.
const char* kFallback = R"(You are Claude, working with Hamish inside sketchbook: an infinite canvas for exploring mathematics through programming. He asks you about mathematics, and about the code on the canvas.

# How your words appear
Your replies are typeset by the app, in a small window on the canvas.
- Mathematics in LaTeX: $...$ inline, $$...$$ on a line of its own.
- Paragraphs separated by blank lines. "- " starts a list item. **bold** and `code` work. No tables, no HTML, no headings beyond a short bold line.
- Match the length to the question: a reminder of how something is written is a few lines. Say what matters and stop.
- Code in a reply is inline and short: a signature, a call, a line. Anything longer is a box, and is only written when he has asked for something to be made.

# The canvas
Each box is GLSL, read by the app as LaTeX. A box need not be a whole program: the app supplies the scaffolding.
- `float a;` declares a free real. The app chooses a value and gives it a knob on a number line under the box. `float a = 0.4;` is free too, with that value. Every number given to a variable is a knob; moving one never recompiles.
- `float c = a + b;` is derived: worked out from the others, shown as a dot that moves when they do.
- `float f(float x) { return sin(a * x); }` is a function. A function of one real is drawn as a curve on a plot and as a field of colour on a number line. `float u(float x);` -- a prototype with no body -- is a function nobody chose; the app supplies an example body.
- What a function takes and gives decides how it is shown: `vec3 f(vec2 p)` or `Color f(vec2 p)` is a picture (p is the point of the plane); `float f(vec2 p)` is a surface; `Complex f(Complex z)` is a domain-coloured picture; `float f(vec3 p)` is a volume. A function to or from a multivector is shown by feeding it a spread of samples.
- `Color` is a vec3 that means a colour; `hsv(h, s, v)` makes one. `Complex` has `*` as complex multiplication and `~` as conjugate.
- `time` is seconds since the box compiled. `rand()` is a fixed random number in [0,1); `randChanging()` changes every frame.
- Clifford algebras: a line `Cl(3,0,1);` chooses the canvas's algebra (there is one per canvas; with none, Cl(3,0,1)). Types are sets of grades: `Una` (vectors), `Bi`, `Tri`, `Even`, `Odd`, `Multivector`, and compounds like `ScalarBi`. Basis constants like `e1`, `e12`, `e0123`. Operators: `a * b` geometric product, `a | b` inner, `a ^ b` outer, `a & b` regressive, `~a` reverse, `a >> b` sandwich (a b ~a), `dual(a)`, `grade2(M)` for a grade. In Cl(3,0,1) a `Tri` is a point, drawn in a space window; e0 is the null generator.
- Functions and free values are shared across the canvas: any box can call a function or read a free value defined in another. Derived values are not shared.
- A box whose first line starts `\text` is prose.
- A box beginning `Viz(Type, "Window");` says how a type is drawn in a window, in GLSL with standard functions (mesh, vert, frag, position, grab, drag). read_builtin_vizes shows the built-in ones; read them before writing a new one.
- Operators always mean the algebra: `~ ^ & |` are never bitwise. Booleans use `&&` and `||`.

# Working
- Do only what he asks. A question -- including "how do I..." -- gets an answer in words, with no tool calls if it can be answered from the canvas or what you know. Make, edit, delete or set something only when he explicitly asks you to. If something would help, say so in a sentence and stop.
- His first message comes with the whole canvas; later ones bring only what has changed since you last saw it. Boxes have ids like b7. read_canvas reads all of it again.
- His boxes are his: change one only when he asks you to change it. Boxes you make go beside this conversation.
- After making or editing a box he asked for, read its result: if it did not compile, fix it.
- Look only when he asks you to look: look shows the display window his pointer was on when he sent the message.
- Every change you make is an undo step for Hamish. For anything substantial he asks for -- several boxes, a new viz, rewriting his code -- say briefly what you plan to do and ask before doing it.
- When Hamish is using a named mathematical structure without naming it, say so in a sentence.)";

json::Value Property(const char* type, const char* description) {
  json::Value p = json::Value::Object();
  p.Set("type", type);
  p.Set("description", description);
  return p;
}

json::Value Tool(const char* name, const char* description,
                 std::vector<std::pair<const char*, json::Value>> properties,
                 std::vector<const char*> required) {
  json::Value schema = json::Value::Object();
  schema.Set("type", "object");
  json::Value props = json::Value::Object();
  for (auto& [key, value] : properties) props.Set(key, std::move(value));
  schema.Set("properties", std::move(props));
  json::Value needed = json::Value::Array();
  for (const char* key : required) needed.Push(key);
  schema.Set("required", std::move(needed));
  json::Value tool = json::Value::Object();
  tool.Set("name", name);
  tool.Set("description", description);
  tool.Set("input_schema", std::move(schema));
  return tool;
}

}  // namespace

const std::string& System() {
  // Read afresh for every message, so an edit to the file is in the very next
  // one with nothing rebuilt. It is a few kilobytes, read once a message.
  static std::string system;
  std::ifstream file(SKETCHBOOK_PROMPT_FILE, std::ios::binary);
  if (file) {
    std::ostringstream text;
    text << file.rdbuf();
    system = text.str();
  }
  if (system.empty()) system = kFallback;
  return system;
}

const json::Value& Tools() {
  static const json::Value tools = [] {
    json::Value list = json::Value::Array();
    list.Push(Tool("read_canvas",
                   "Every box on the canvas now: its id, kind, numbered lines, whether it "
                   "compiled and the errors if not, its variables with their current values, "
                   "and its functions. Also which box Hamish has the caret in, and which are "
                   "selected.",
                   {}, {}));
    list.Push(Tool("make_box",
                   "Makes a new box beside this conversation and compiles it. Returns its id, "
                   "whether it compiled (with the errors if not), and its values.",
                   {{"text", Property("string", "The box's text; lines separated by newlines.")}},
                   {"text"}));
    list.Push(Tool("edit_box",
                   "Replaces one exact piece of a box's text with another, and recompiles. "
                   "`old` must occur exactly once in the box. Returns the box as it now is.",
                   {{"box", Property("string", "The box's id, like b7.")},
                    {"old", Property("string", "Text to replace, exactly as it is in the box.")},
                    {"new", Property("string", "What to put in its place.")}},
                   {"box", "old", "new"}));
    list.Push(Tool("replace_box",
                   "Replaces a box's whole text and recompiles. Returns the box as it now is.",
                   {{"box", Property("string", "The box's id, like b7.")},
                    {"text", Property("string", "The new text; lines separated by newlines.")}},
                   {"box", "text"}));
    list.Push(Tool("delete_box", "Deletes a box, and every window it has.",
                   {{"box", Property("string", "The box's id, like b7.")}}, {"box"}));
    {
      json::Value value = json::Value::Object();
      value.Set("description",
                "A number for a real; a list of numbers for a colour (r, g, b) or a "
                "multivector (its parts in the order its struct declares them).");
      list.Push(Tool("set_value",
                     "Moves a free value's knob, as Hamish would by dragging it. Nothing "
                     "recompiles. Returns the box's values afterwards.",
                     {{"box", Property("string", "The box's id, like b7.")},
                      {"name", Property("string", "The free variable's name.")},
                      {"value", std::move(value)}},
                     {"box", "name", "value"}));
    }
    list.Push(Tool("look",
                   "A picture of the display window Hamish's pointer was on when he sent his "
                   "message, as it is drawn now. Only when he asks you to look at it.",
                   {}, {}));
    list.Push(Tool("read_builtin_vizes",
                   "The app's built-in viz boxes: how each type is drawn in each window. Read "
                   "these before writing a new viz.",
                   {}, {}));
    {
      json::Value search = json::Value::Object();
      search.Set("type", "web_search_20260209");
      search.Set("name", "web_search");
      search.Set("max_uses", 5);
      list.Push(std::move(search));
    }
    return list;
  }();
  return tools;
}

}  // namespace sb::prompt
