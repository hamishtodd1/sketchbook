// Cards that live on the canvas.
//
// A code box is the centre of the program. Each of its lines is independently
// either LaTeX (the mathematician's reading of that line), code being previewed
// under the pointer, or code being edited. Underneath, the whole box is one
// translation unit whose free variables are driven by number lines.
#pragma once

#include <future>
#include <string>
#include <utility>
#include <vector>

#include "compiler.h"
#include "fontstore.h"
#include "mathline.h"
#include "mathprogram.h"
#include "raylib.h"
#include "tex/tex.h"
#include "viewport.h"

namespace sb {

class CodeBlock {
 public:
  CodeBlock(Vector2 worldTopLeft, float worldEm, std::vector<std::string> lines);

  // --- geometry -----------------------------------------------------------
  // Rows vary in height: a fraction needs more room than a line of code, so the
  // box expands and contracts as lines flip between the two readings.
  struct Row {
    float y = 0.0f;        // top, screen space
    float height = 0.0f;
    float baseline = 0.0f; // where text sits within the row
    bool  asLatex = false;
  };
  struct Geometry {
    float            zoom = 1.0f;
    float            emPx = 0.0f;
    float            advance = 0.0f;   // one monospaced cell
    float            lineHeight = 0.0f;
    float            ascent = 0.0f;
    Rectangle        rect{};
    Vector2          textLeftTop{};
    std::vector<Row> rows;
  };
  const Geometry& Measure(const Viewport& viewport, FontStore& fonts) const;

  bool    Contains(Vector2 screenPoint, const Viewport& viewport, FontStore& fonts) const;
  // The grab band around the edge, which moves the whole window.
  bool    OnRim(Vector2 screenPoint, const Viewport& viewport, FontStore& fonts) const;
  void    SetDragging(bool dragging) { dragging_ = dragging; }
  void    PlaceCaret(Vector2 screenPoint, const Viewport& viewport, FontStore& fonts);
  // Bottom-left of the card, where this box's number lines dock.
  Vector2 DockAnchor(const Viewport& viewport, FontStore& fonts) const;
  float   DockWidth(const Viewport& viewport, FontStore& fonts) const;

  // --- editing ------------------------------------------------------------
  void SetFocused(bool focused);
  bool Focused() const { return focused_; }
  bool Empty() const;
  bool Dirty() const { return dirty_; }

  void HandleInput();
  void InsertCodepoint(int codepoint);
  std::string Text() const;
  const std::vector<std::string>& Lines() const { return lines_; }

  // The buttons that appear on a focused box. Returns true if it took the
  // click, so nothing behind it also reacts.
  bool HandleChrome(const Viewport& viewport, FontStore& fonts);
  bool CloseRequested() const { return closeRequested_; }

  // --- the live program ---------------------------------------------------
  void RequestCompile();
  void Poll();
  compiler::Status Status() const { return outcome_.status; }

  const std::vector<mathprogram::Variable>& Variables() const { return program_.variables(); }
  const mathprogram::Variable* FindVariable(const std::string& name) const;

  // Every variable in this box has a colour of its own, taken from where it
  // stands in the box. Everything that represents that variable wears it: the
  // handle on its number line, its curve on a plot, and the box drawn round its
  // name here.
  Color VariableColor(const std::string& name) const;
  // The variables to draw a box round this frame -- set by whichever window is
  // being hovered or dragged, and refilled every frame.
  void  SetHighlights(std::vector<std::string> names);
  void SetVariableValue(const std::string& name, float value);
  // Knob positions from a loaded canvas, adopted on the next rebuild.
  void RestoreValues(std::vector<std::pair<std::string, float>> values);

  // --- persistence --------------------------------------------------------
  Vector2 WorldPosition() const { return origin_; }
  float   Em() const { return em_; }
  bool    ShowMath() const { return showMath_; }
  void    SetShowMath(bool value) { showMath_ = value; }
  void    SetWorldPosition(Vector2 position) { origin_ = position; }

  // --- drawing ------------------------------------------------------------
  void Draw(const Viewport& viewport, FontStore& fonts) const;
  void DrawOverlay(const Viewport& viewport, FontStore& fonts) const;

 private:
  // Per line: its mathematical reading, and the typeset form at the current
  // size. Rebuilt only when the text or the zoom level actually changes.
  struct LineCache {
    std::string    source;
    mathline::Line parsed;
    tex::Parsed    tex;
    float          emPx = -1.0f;
    tex::Layout    layout;
  };

  struct Snapshot {
    std::vector<std::string> lines;
    int                      caretLine = 0;
    int                      caretCol = 0;
  };

  // What kind of edit produced the current state, so consecutive keystrokes of
  // the same kind collapse into one undo step.
  enum class EditKind { None, Insert, Delete, Structural };

  struct Chrome {
    Rectangle toggle{};
    Rectangle close{};
  };

  void   EnsureCache(FontStore& fonts, float emPx) const;
  // Where `name` is written on this line, in whichever reading is showing. One
  // rectangle per mention: a variable can appear more than once.
  std::vector<Rectangle> MentionRects(std::size_t line, const std::string& name,
                                      const Geometry& g, FontStore& fonts) const;
  int    RowAt(Vector2 screenPoint, const Viewport& viewport, FontStore& fonts) const;
  bool   ShowsCode(int line) const;
  // 1-based line the current diagnostic points at, or 0 if there is none.
  int    ErrorLine() const;
  std::string Injection(int line) const;
  Chrome ChromeRects(const Geometry& g) const;

  void   PushUndo(EditKind kind);
  void   Undo();

  void   InsertNewline();
  void   DeleteBefore();
  void   DeleteAt();
  void   MoveCaret(int dLine, int dChar);
  void   MoveCaretByWord(int direction);
  void   ClampCaret();
  void   TouchEdit();

  Vector2                  origin_;
  float                    em_;
  std::vector<std::string> lines_;

  bool   focused_ = false;
  // When focused, the box shows all code. The toggle flips it back to reading
  // as mathematics while still being editable line by line.
  bool   showMath_ = false;
  bool   closeRequested_ = false;
  int    caretLine_ = 0;
  int    caretCol_ = 0;   // byte offset within lines_[caretLine_]
  double blinkAnchor_ = 0.0;
  bool   dirty_ = false;
  // Resolved during Draw, before the geometry that depends on it is measured.
  mutable int hoverLine_ = -1;
  mutable bool rimHot_ = false;
  bool         dragging_ = false;

  std::vector<std::string> highlights_;

  std::vector<Snapshot> undo_;
  EditKind              lastEdit_ = EditKind::None;
  double                lastEditTime_ = -1.0;

  mathprogram::Program program_;

  // Legacy path: a box that declares no variables but does define
  // `bool testFunc()` still gets its red/blue square.
  compiler::Outcome              outcome_;
  std::future<compiler::Outcome> pending_;

  mutable std::vector<LineCache> cache_;
  mutable Geometry               geometry_;
};

// A box of typeset mathematics, display only.
class MathBlock {
 public:
  MathBlock(Vector2 worldTopLeft, float worldEm, std::string latex, bool displayStyle = true);

  void Draw(const Viewport& viewport, FontStore& fonts) const;

  const std::string& source() const { return source_; }
  const std::string& error() const { return parsed_.error; }

 private:
  Vector2     origin_;
  float       em_;
  std::string source_;
  bool        display_;
  tex::Parsed parsed_;
};

}  // namespace sb
