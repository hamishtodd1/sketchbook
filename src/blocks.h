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

class CodeBlock;

// A variable whose window is being used, and the point on that window where it
// is shown. Everything standing for a variable wears its colour; this is what
// says *which* window is in use, so the name can be boxed where it is written
// and joined to the window by a curve.
//
// `from` is the visualization of the value, not the pointer: the handle on a
// number line, the picture on a plot. The one exception is a shader's preview,
// where the pointer really is the value it is reporting.
struct ActiveValue {
  CodeBlock*  block = nullptr;
  std::string name;
  Vector2     from{};   // screen space
  // The canvas is drawing this one's whole derivation -- the wires from the
  // values it is made of, through the operators, to here -- so the box must not
  // also draw the plain wire from its window. The names still light up.
  bool        wired = false;
};

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
  // From the card's corner to the first character, in world units. It is the
  // *text* that lands on the grid rather than the corner -- with a dot for
  // every line, a box put down has its lines between the rows of dots -- and
  // this is the difference between the two.
  Vector2      TextOffset() const;

  // --- editing ------------------------------------------------------------
  void SetFocused(bool focused);
  bool Focused() const { return focused_; }
  bool Empty() const;
  bool Dirty() const { return dirty_; }

  void HandleInput();
  void InsertCodepoint(int codepoint);

  // --- selecting text ------------------------------------------------------
  //
  // Dragging across a box's text selects it. That is worth having on its own,
  // but the reason it exists is the gesture built on it: highlight part of
  // where a variable is written and drag it out, and that variable gets a
  // window of its own.
  void        BeginSelection(Vector2 screenPoint, const Viewport& viewport, FontStore& fonts);
  void        ExtendSelection(Vector2 screenPoint, const Viewport& viewport, FontStore& fonts);
  bool        HasSelection() const;
  void        SelectNone() { anchorLine_ = caretLine_; anchorCol_ = caretCol_; }
  bool        PointInSelection(Vector2 screenPoint, const Viewport& viewport,
                               FontStore& fonts) const;
  // The variable the selection lies inside, if this box declares one by that
  // name. Empty if the selection spans more than a name, or names nothing.
  std::string SelectedVariable() const;
  // Every variable the highlight touches. Taking hold of a name in the text is
  // the same statement as taking hold of its window, so it lights up the same
  // things: the name wherever it is written, the wire, and the viz blinking at
  // the other end. Empty when nothing is highlighted.
  std::vector<std::string> HighlightedVariables() const;
  std::string Text() const;
  // What the highlight covers, as it is written -- newlines and all.
  std::string SelectedText() const;

  // --- cut, copy and paste, in the text ------------------------------------
  //
  // On the *system* clipboard rather than one of our own, so that a line of
  // this canvas can go into a message and a paragraph from somewhere else can
  // come in. Which clipboard Ctrl+C means is decided by the caret: a box being
  // typed into copies its text, and a canvas with no caret in it copies the
  // windows that are selected. One key, two clipboards, one rule.
  void CopyText(bool cut);
  void PasteText();
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

  // --- what is being talked about -----------------------------------------
  //
  // A variable is mutable unless it is const, so it can hold one value on one
  // line and a different one further down. With no caret in this box its
  // windows show what the program *ended* with -- every value is printed after
  // the last of your lines, so the end of the block is what a window means.
  //
  // With a caret the box is being read a line at a time, and only the variables
  // written on that line still have anything to say. Highlight a stretch and it
  // is every variable the highlight touches instead. Everything else this box
  // declares stands its windows down: they would be showing a number that has
  // nothing to do with where you are looking.
  bool InScope(const std::string& name) const;

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

  // --- making a variable from a number line --------------------------------
  //
  // A number line is where values live, so it is also where a new one is made.
  // Pointing at the empty part of an axis is an offer to put a value in the box
  // that axis belongs to, and this box answers by blinking a caret where the
  // declaration would land. Clicking writes it.
  //
  // What is being pointed at is a *line* rather than a place between two
  // characters, which is why a line reading as mathematics can carry the mark
  // too: there the caret sits at the end of the typeset row.
  void SetInsertCaret(bool value);
  // Where a declaration would land: the line the caret was last left on, or the
  // last line of the box if the caret has never been put anywhere.
  int  InsertLine() const;
  // Writes one there -- on that line if it is blank, on a new line just under
  // it otherwise -- and leaves the caret after it.
  void InsertDeclaration(const std::string& declaration);
  // A name nothing in this box is using yet.
  std::string FreshName() const;

  // Writes a free variable's present value into the source, so that it keeps
  // that value with no knob to hold it. Throwing away a control has to leave
  // the thing it controlled somewhere. Returns true if the text changed.
  bool BakeVariable(const std::string& name);

  // --- a box that draws rather than computes -------------------------------
  //
  // `Viz(Real);` makes this box a **viz**: it says how a value of that type is
  // drawn inside a display window rather than declaring a value of its own. Its
  // C++ half lays down a mesh; its GLSL half is the vertex shader that places
  // one. See viz.h for the vocabulary and for how the two halves are paired.
  const std::string&        VizType() const { return program_.vizType(); }
  const std::vector<float>& VizMesh() const { return program_.vizMesh(); }
  const std::vector<float>& VizLines() const { return program_.vizLines(); }
  // Whether C++ would have this box. A viz box that compiles is the mesh half;
  // one that does not is the shader half.
  bool BuildsAsCxx() const;
  // What the canvas says about how its types are drawn. A viz's lines become
  // part of every program that has a value of that type, so a box has to be
  // told where to find them. Owned by the canvas, which outlives every box.
  void SetVizSources(const mathprogram::VizSources* sources) { vizSources_ = sources; }
  bool                      HasVizShader() const { return haveVizShader_; }
  Shader                    VizShader() const { return vizShader_; }
  int                       VizUniform(const char* name) const;

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

  // --- wiring --------------------------------------------------------------
  //
  // Where this box writes a name, and how a value it defines is computed. A
  // variable defined out of other values is not one thing pointing at another;
  // it is the end of a little machine, and these are what that machine is drawn
  // from.
  std::vector<Rectangle> MentionsOf(const std::string& name, const Viewport& viewport,
                                    FontStore& fonts) const;
  mathline::Flow         FlowFor(const std::string& name) const;

  // Every variable in this box has a colour of its own, taken from where it
  // stands in the box. Everything that represents that variable wears it: the
  // handle on its number line, its curve on a plot, and the box drawn round its
  // name here.
  Color VariableColor(const std::string& name) const;
  // The variables to draw a box round this frame -- set by whichever window is
  // being hovered or dragged, and refilled every frame.
  void  SetHighlights(std::vector<ActiveValue> values);
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
  // The strip of controls above a box. Drawn apart from the box, and after
  // every box, so that a neighbouring card can never be on top of it.
  void DrawChrome(const Viewport& viewport, FontStore& fonts) const;
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
    // One control, not three: left, centred and right are one choice, and a
    // choice with three states is a thing you press until it says what you
    // want. The icon shows the state it is in, so it is also its own label.
    Rectangle justify{};
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
  // Which Clifford algebra this box works in, read off its own lines. It
  // decides what counts as a type name, how one is written as mathematics, and
  // what an example of one looks like -- so every one of those has to ask.
  clifford::Signature Algebra() const;
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
  // The row a position in the source is drawn in, or -1. Takes the position
  // rather than reading the caret, since the caret is not the only thing that
  // wants pointing at a line.
  int         RowOf(const Geometry& g, int line, int col) const;
  int         CaretRow(const Geometry& g) const { return RowOf(g, caretLine_, caretCol_); }
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
  // The selection in reading order, whichever way round it was made.
  void   SelectionRange(int* fromLine, int* fromCol, int* toLine, int* toCol) const;
  // Whether `name` is written as a whole word anywhere between two positions.
  // `wholeLines` ignores the columns and takes every line in the range entire.
  bool   MentionedBetween(const std::string& name, int fromLine, int fromCol, int toLine,
                          int toCol, bool wholeLines) const;
  // The slice of a row that is selected, as byte offsets, or false for none.
  bool   SelectionInRow(const Row& row, std::size_t* from, std::size_t* to) const;
  // Takes the selected text out. True if there was any.
  bool   EraseSelection();

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
  // The other end of the selection. Equal to the caret when there is none, so
  // "is anything selected" is one comparison and needs no extra flag.
  int    anchorLine_ = 0;
  int    anchorCol_ = 0;
  // Whether the caret has ever been put somewhere on purpose. Until it has,
  // "where the caret was left" means the end of the box.
  bool   caretUsed_ = false;
  // A number line is offering to write a value into this box this frame. Set
  // from outside every frame, like the highlights: it is a fact about where the
  // pointer is, not something to remember.
  bool   insertCaret_ = false;
  double blinkAnchor_ = 0.0;
  bool   dirty_ = false;
  // Resolved during Draw, before the geometry that depends on it is measured.
  mutable int hoverLine_ = -1;
  mutable bool rimHot_ = false;
  bool         dragging_ = false;

  std::vector<ActiveValue> highlights_;

  std::vector<Snapshot> undo_;
  EditKind              lastEdit_ = EditKind::None;
  double                lastEditTime_ = -1.0;

  mathprogram::Program program_;
  const mathprogram::VizSources* vizSources_ = nullptr;

  // The GPU side of a box whose code turned out to be GLSL: the shader itself,
  // the uniform slots its knobs drive, and a blank surface for it to cover.
  Shader                                shader_{};
  bool                                  haveShader_ = false;
  // A viz box's vertex shader, which is a different thing from the fragment
  // shader above: it places a mesh rather than colouring a square.
  Shader                                vizShader_{};
  bool                                  haveVizShader_ = false;
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
