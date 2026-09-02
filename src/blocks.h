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
  ~CodeBlock();

  // --- geometry -----------------------------------------------------------
  // Rows vary in height: a fraction needs more room than a line of code, so the
  // box expands and contracts as lines flip between the two readings.
  // One row *as drawn*, which is not the same as one line of source: prose
  // wraps, so a long line becomes several rows. A code box always has exactly
  // one row per line.
  struct Row {
    float y = 0.0f;        // top, screen space
    float height = 0.0f;
    float baseline = 0.0f; // where text sits within the row
    // Where this row's content starts, and how wide it is. Code always starts
    // at the left; prose starts wherever its justification puts it.
    float x = 0.0f;
    float width = 0.0f;
    bool  asLatex = false;
    // Which line this row came from, and which slice of it. The slices of a
    // line partition it, so every byte -- and so every caret position -- is in
    // exactly one row.
    int         line = 0;
    int         piece = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
  };
  struct Geometry {
    float            zoom = 1.0f;
    float            emPx = 0.0f;
    float            advance = 0.0f;   // one monospaced cell
    float            lineHeight = 0.0f;
    float            ascent = 0.0f;
    float            measure = 0.0f;   // how far prose runs before it wraps, px
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

  // --- prose ---------------------------------------------------------------
  //
  // A box whose first line begins with 	ext is not code: it is words, and it
  // is set as words -- upright roman on the bare canvas, with no card behind
  // it, because a caption on a slide is ink rather than a window. Writing the
  // sigil is what turns a code box into one, and deleting it turns it back, so
  // there is one gesture and it is reversible.
  //
  // Boldness, italics and justification are the box's own, not the text's:
  // there is no markup to learn and nothing to get half-applied.
  enum class Justify : int { Left = 0, Centre = 1, Right = 2 };
  bool    IsText() const;
  // The measure: how wide the words are allowed to run before they wrap, in
  // world units. Prose needs a chosen width -- a paragraph is a shape, not a
  // list of lines -- and it is the one dimension the wheel does not decide.
  float   TextWidth() const { return textWidth_; }
  void    SetTextWidth(float value) { textWidth_ = value; }
  bool    Bold() const { return bold_; }
  bool    Italic() const { return italic_; }
  Justify Justification() const { return justify_; }
  void    SetBold(bool value) { bold_ = value; }
  void    SetItalic(bool value) { italic_ = value; }
  void    SetJustification(Justify value) { justify_ = value; }

  // --- resizing --------------------------------------------------------------
  // The wheel resizes a box uniformly, which for a box of type means the type
  // gets bigger. `anchorWorld` is the corner that must not move.
  Rectangle WorldBounds(const Viewport& viewport, FontStore& fonts) const;
  void      Scale(float factor, Vector2 anchorWorld);
  // The padding either side of the words, in world units, at a given type size.
  // A caller fitting a measure into a space has to allow for it.
  static float PaddingX(float em);

  // --- editing ------------------------------------------------------------
  void SetFocused(bool focused);
  bool Focused() const { return focused_; }
  bool Empty() const;
  bool Dirty() const { return dirty_; }

  void HandleInput();
  void InsertCodepoint(int codepoint);
  std::string Text() const;
  const std::vector<std::string>& Lines() const { return lines_; }

  // The button that appears on a focused box. Returns true if it took the
  // click, so nothing behind it also reacts.
  bool HandleChrome(const Viewport& viewport, FontStore& fonts);

  // A box being carried onto the drop target stops being drawn -- it and
  // everything docked under it -- so that it reads as absorbed.
  void SetAbsorbed(bool absorbed) { absorbed_ = absorbed; }
  bool Absorbed() const { return absorbed_; }

  // Caught by the lasso. A selected window wears its rim, moves with the rest
  // of the selection, and goes with it.
  void SetSelected(bool selected) { selected_ = selected; }
  bool Selected() const { return selected_; }

  // --- closing ------------------------------------------------------------
  //
  // Closing a window is remembered, or the next Sync would put it straight
  // back. Kept on the box because it is a fact about one of its variables.
  void Hide(const std::string& name);
  void Unhide(const std::string& name);
  bool IsHidden(const std::string& name) const;
  const std::vector<std::string>& Hidden() const { return hidden_; }
  void RestoreHidden(std::vector<std::string> names) { hidden_ = std::move(names); }

  void HideShader() { shaderHidden_ = true; }
  bool ShaderHidden() const { return shaderHidden_; }
  void SetShaderHidden(bool hidden) { shaderHidden_ = hidden; }

  // Writes a free variable's present value into the source, so that it keeps
  // that value with no knob to hold it. Throwing away a control has to leave
  // the thing it controlled somewhere. Returns true if the text changed.
  bool BakeVariable(const std::string& name);

  // Draws this box's shader into a rectangle. The window that owns that
  // rectangle lives elsewhere; the GPU side of it lives here.
  bool HasShader() const { return haveShader_; }
  void DrawShaderInto(Rectangle rect) const;
  void SetProbePoint(Vector2 point) const { probePoint_ = point; }

  // --- the live program ---------------------------------------------------
  void RequestCompile();
  void Poll();
  // Frees whatever this box holds on the GPU. Called while the window is still
  // open, because a destructor may well run after the context has gone.
  void ReleaseGpu();
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
  bool    ShowSource() const { return showSource_; }
  void    SetShowSource(bool value) { showSource_ = value; }
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
    // What is actually typeset: mathline's reading of the code, or, in a text
    // box, the line's own words wrapped in \text.
    std::string    latex;
    unsigned       styleSig = 0;   // bold/italic, which change the words' face
    tex::Parsed    tex;
    float          emPx = -1.0f;
    tex::Layout    layout;

    // Prose only: the line broken to the measure. The pieces partition the
    // line, so `begin` plus the piece's length is the next piece's `begin` and
    // every caret position lands in exactly one of them.
    float                    wrapWidth = -1.0f;
    std::vector<std::size_t> pieceBegin;
    std::vector<std::size_t> pieceEnd;
    std::vector<tex::Layout> pieceLayout;
  };

  struct Snapshot {
    std::vector<std::string> lines;
    int                      caretLine = 0;
    int                      caretCol = 0;
  };

  // What kind of edit produced the current state, so consecutive keystrokes of
  // the same kind collapse into one undo step.
  enum class EditKind { None, Insert, Delete, Structural };

  // The strip of controls above a box. A code box gets the one button that
  // flips it between readings; a text box gets the ones that decide how its
  // words are set.
  struct Chrome {
    bool      forText = false;
    Rectangle toggle{};
    Rectangle bold{};
    Rectangle italic{};
    Rectangle justify[3]{};
  };

  void   EnsureCache(FontStore& fonts, float emPx, float measurePx) const;
  // Where `name` is written in this row, in whichever reading it is showing.
  // One rectangle per mention: a variable can appear more than once.
  std::vector<Rectangle> MentionRects(std::size_t rowIndex, const std::string& name,
                                      const Geometry& g, FontStore& fonts) const;
  int    RowAt(Vector2 screenPoint, const Viewport& viewport, FontStore& fonts) const;
  bool   ShowsCode(int line) const;
  // 1-based line the current diagnostic points at, or 0 if there is none.
  int    ErrorLine() const;
  std::string Injection(int line) const;
  Chrome ChromeRects(const Geometry& g) const;
  // The bar just inside the right edge that sets the measure. Only a text box
  // has one; it is inset clear of the rim's grab band, like a picture's crop
  // corners, so the two gestures never want the same pixel.
  Rectangle MarginHandle(const Geometry& g) const;
  // A code box shows its chrome only while it has the caret. A text box shows
  // its controls whenever you go looking for them, the way a rim appears.
  bool   ChromeVisible(const Geometry& g, Vector2 pointer) const;
  // How many bytes at the front of a line are the \text sigil rather than
  // words. Non-zero only on the first line. The caret may not go before it: it
  // is not part of what you are writing.
  std::size_t ProseOffset(std::size_t line) const;
  // The words on a line, with the sigil taken off, and the LaTeX that sets a
  // piece of them.
  std::string Prose(std::size_t line) const;
  std::string TextLatex(std::string_view words) const;
  // The face those words are set in, which is what measures them.
  Face        ProseFace() const;
  // The row the caret is in, or -1.
  int         CaretRow(const Geometry& g) const;
  // How far into a row a byte offset sits, and the offset nearest an x. Read
  // off the cached layout, so the arrow keys can use them without a font store.
  float       RowOffsetX(const Row& row, std::size_t col) const;
  std::size_t RowColAt(const Row& row, float x) const;

  // Compiles the GLSL the program assembled. Must be called on the thread that
  // owns the graphics context, which is why the program cannot do it itself.
  void   CompileShader();
  // Renders the probe shader into its one-row target and reads the pixels back,
  // which is how a shader box gets values onto its number lines. Only when
  // something it depends on has moved: a read is a stall.
  void   ProbeIfNeeded();

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
  // How the words of a text box are set. Ignored by a code box.
  bool    bold_ = false;
  bool    italic_ = false;
  Justify justify_ = Justify::Left;
  float   textWidth_ = 0.0f;   // world units; 0 means "pick one from the size"
  // A text box is edited as words, not as source. This is the toggle that shows
  // the source anyway -- which is the only way to see the sigil, and so the
  // only way to take it back out. It stays where it is put: a box can be left
  // reading as its own source, and it is only the *button* that goes when the
  // box stops being pointed at.
  bool    showSource_ = false;
  // Dragging the measure.
  bool    marginDrag_ = false;
  // When focused, the box shows all code. The toggle flips it back to reading
  // as mathematics while still being editable line by line.
  bool   showMath_ = false;
  bool   absorbed_ = false;
  bool   selected_ = false;
  std::vector<std::string> hidden_;
  bool   shaderHidden_ = false;
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

  // The GPU side of a box whose code turned out to be GLSL: the shader itself,
  // the uniform slots its knobs drive, and a blank surface for it to cover.
  Shader                                shader_{};
  bool                                  haveShader_ = false;
  std::vector<std::pair<std::string, int>> uniforms_;
  Texture2D                             previewSurface_{};
  bool                                  havePreviewSurface_ = false;

  // The second shader, the one that answers questions instead of drawing, and
  // the one-pixel-per-variable target it answers into.
  Shader                                probeShader_{};
  bool                                  haveProbe_ = false;
  std::vector<std::pair<std::string, int>> probeUniforms_;
  int                                   probePointLocation_ = -1;
  RenderTexture2D                       probeTarget_{};
  bool                                  haveProbeTarget_ = false;
  // Where in the picture the numbers are read: under the pointer while you are
  // over it, its middle otherwise. Resolved while drawing.
  mutable Vector2                       probePoint_{0.5f, 0.5f};
  std::vector<float>                    probeSignature_;

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
