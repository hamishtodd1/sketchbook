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

// A function, drawn by being fed.
//
// A function has no one place to be, so it is not shown as a value: a spread of
// samples of its input space is drawn in whatever window that type goes in,
// every one of them is put through the function, and the answers are drawn in
// whatever window the output type goes in -- which may well be the same window.
// The two runs are the same length and line up index for index, which is what
// lets pointing at one end light up the other.
struct Sampling {
  std::string name;         // the function being shown
  std::string inputType;
  std::string outputType;
  int         count = 0;
  int         inStride = 1;
  int         outStride = 1;
  std::vector<float> inputs;    // count * inStride
  std::vector<float> outputs;   // count * outStride
  bool        ready = false;    // the answers have come back at least once

  bool Live() const { return ready && count > 0; }
};

class CodeBlock {
 public:
  CodeBlock(Vector2 worldTopLeft, float worldEm, std::vector<std::string> lines);
  ~CodeBlock();

  // Everything the geometry is a function of.
  //
  // `Measure` is asked for a dozen times a frame per box -- by every panel, by
  // the placement, by every hit test -- and recomputing it each time was half
  // of all the CPU this app spent. So the answer is kept, and this is what says
  // whether it is still the answer. Two things are deliberately *not* in here
  // because they are read off the lines and so cannot move without `cache`
  // moving: which algebra the box works in, and whether it is prose.
  struct Shape {
    float    zoom = -1.0f;
    float    emPx = -1.0f;
    float    measure = -1.0f;
    float    left = 0.0f;      // where it lands, which only translates it
    float    top = 0.0f;
    unsigned cache = 0;        // bumped whenever a line's reading is rebuilt
    unsigned flags = 0;        // the toggles that decide how a row is read
    int      caret = -1;
    int      hover = -1;
    int      error = 0;
    unsigned values = 0;       // what the injected numbers would say

    // Everything but where it sits. A pan moves a box without reshaping it, so
    // that case is worth telling apart: the rows are shifted rather than built.
    bool SameShape(const Shape& other) const {
      return zoom == other.zoom && emPx == other.emPx && measure == other.measure &&
             cache == other.cache && flags == other.flags && caret == other.caret &&
             hover == other.hover && error == other.error && values == other.values;
    }
  };

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

  // Closing is remembered, or the next Sync would put the window straight back.
  // One flag per *kind* of window, because a box can define a picture and a
  // surface at once and closing one of them must not take the other with it.
  enum Drawn : int { kPicture = 0, kSurface = 1, kVolume = 2, kDrawnKinds = 3 };
  void HideDrawn(int kind) { drawnHidden_[kind] = true; }
  bool DrawnHidden(int kind) const { return drawnHidden_[kind]; }
  void SetDrawnHidden(int kind, bool hide) { drawnHidden_[kind] = hide; }

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
  // --- drawing one of this box's functions ---------------------------------
  //
  // A function is GLSL, so a window showing one *calls* it rather than reading
  // samples of it. The box owns the shader because the box owns the functions;
  // the window says which one, over what range, and in which of the two shapes
  // a function is drawn in.
  bool HasFieldShader() const { return haveShader_; }

  // --- executed up to here -------------------------------------------------
  //
  // A variable is mutable, so a window showing one has to say *when*. This is
  // that when: the line the box is being read at. It is where the caret is, or
  // where the pointer is over the box, and for a box nobody has touched this
  // session it is the end -- which is what the whole block came to.
  int  ExecutedUpTo() const;
  void SetPointerRow(int row) { pointerRow_ = row; }
  // `mode` is 0 for a field across an axis and 1 for a curve on a plot.
  // Returns false if there is nothing to draw with, in which case nothing was
  // begun and EndField must not be called.
  bool BeginField(const std::string& name, int mode, Vector2 range, Vector2 vertical,
                  float halfWidth) const;
  void EndField() const;

  // --- what a box draws ----------------------------------------------------
  //
  // **A box draws because it defines a function**, not because it writes a
  // colour, and *what kind of window* it gets is decided by how many variables
  // that function takes. One is a curve; two is a square of pixels; three is a
  // volume. Each is a different kind of display window, because each has a
  // different camera -- none, an orthographic one, a perspective one -- and a
  // window is largely its camera.
  //
  // Each of these is the function of that kind the box is being read inside, or
  // the first one it defines if the caret is elsewhere.
  int FlatPictureFunction() const;   // two in, a colour or a complex number out
  int SurfaceFunction() const;       // two in, a real out
  int VolumeFunction() const;        // three in
  bool Draws() const { return FlatPictureFunction() >= 0; }
  bool HasSurface() const { return SurfaceFunction() >= 0; }
  bool HasVolume() const { return VolumeFunction() >= 0; }

  // --- a function, drawn by being fed --------------------------------------
  //
  // Which function this box is being read *inside*, or -1. That is the whole
  // trigger: put the caret on a signature, on a `return`, or on either of the
  // braces, and the function those lines belong to is the one shown.
  int FunctionShown() const;
  // What is being drawn for it, empty unless a function is being shown and its
  // samples have come back.
  const Sampling& Samples() const { return sampling_; }
  const std::vector<mathprogram::Function>& Functions() const { return program_.functions(); }
  // The types this box's sampled functions take and give. A window for one of
  // them exists whether or not that function is the one being read, so that
  // nothing shuffles about as the caret moves.
  std::vector<std::string> SampledTypes() const;
  // The spread to feed it, from whatever box says how this type is spread out.
  // Given from outside because a sampler is a box like any other and may be
  // anywhere on the canvas.
  void SetSampleInputs(const std::vector<float>& inputs, int count, int stride);
  // Which sample the pointer is on, set by whichever window is showing them and
  // cleared every frame. It lights up at both ends and a wire joins them.
  void SetSampleHover(int index) const { sampleHover_ = index; }
  int  SampleHover() const { return sampleHover_; }

  // A sampler box -- `Samples(float, 24);` -- says how a type's space is spread
  // out. It holds a compute program that lays the spread down, and this is what
  // runs it. Empty until it has compiled.
  const std::string& SamplerType() const { return program_.samplerType(); }
  int                SamplerStride() const { return program_.samplerStride(); }
  const std::vector<float>& SamplerRun() const;

  const std::string&        VizType() const { return program_.vizType(); }
  // How much geometry the last run laid down, and where it is. The vertices
  // never come back to the CPU: what is here is the count, which is all a draw
  // call needs to know.
  int          VizTriCount() const { return vizTriCount_; }
  int          VizLineCount() const { return vizLineCount_; }
  unsigned int VizTriBuffer() const { return vizTriBuffer_; }
  unsigned int VizLineBuffer() const { return vizLineBuffer_; }
  // A viz box whose half lays geometry down: it holds a compute program and the
  // buffers that program writes into. Running it for a value is what produces
  // the mesh; it only actually dispatches when the value has changed.
  bool HasVizMesh() const { return vizMeshProgram_ != 0; }
  void RunVizMesh(float value) const;
  bool                      HasVizShader() const { return haveVizShader_; }
  Shader                    VizShader() const { return vizShader_; }
  int                       VizUniform(const char* name) const;

  // Draws this box's shader into a rectangle. The window that owns that
  // rectangle lives elsewhere; the GPU side of it lives here.
  bool HasShader() const { return haveShader_; }
  // `centre` and `span` are the patch of the input space each covers. `orbit`
  // is where the eye is: yaw, pitch above the plane, and how far off -- which
  // only a perspective camera has any use for.
  void DrawPictureInto(Rectangle rect, Vector2 centre, float span) const;
  void DrawSurfaceInto(Rectangle rect, Vector2 centre, float span, Vector3 orbit) const;
  void DrawVolumeInto(Rectangle rect, Vector2 centre, float span, Vector3 orbit) const;
  void SetProbePoint(Vector2 point) const { probePoint_ = point; }

  // --- the live program ---------------------------------------------------
  void RequestCompile();
  void Poll();
  // Frees whatever this box holds on the GPU. Called while the window is still
  // open, because a destructor may well run after the context has gone.
  void ReleaseGpu();

  const std::vector<mathprogram::Variable>& Variables() const { return program_.variables(); }
  // Which Clifford algebra this box works in, read off its own lines. It
  // decides what counts as a type name, how one is written as mathematics, and
  // what an example of one looks like -- so every one of those has to ask.
  clifford::Signature Algebra() const;

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
  // How a box reads, and whether that was decided or merely defaulted.
  //
  // **Auto** is where a box starts: mathematics while it is being read, and all
  // code while it is being written, which is the rule that "editing a box means
  // editing all of it" states. The toggle does not so much flip between two
  // states as *settle* the question -- once pressed, the box reads that way
  // whether or not it has the caret. A toggle that springs back the moment you
  // click away has not done anything.
  enum class Reading : int { Auto = 0, Maths = 1, Code = 2 };
  int  ReadingMode() const { return static_cast<int>(reading_); }
  void SetReadingMode(int mode) { reading_ = static_cast<Reading>(mode); }
  // Whether the whole box is code right now, which is what the button has to
  // ask before it can say what pressing it would do.
  bool ReadsAsCode() const;
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
    // Which function the line was read inside, since that is now part of what
    // decides its reading: editing a signature has to re-read the returns under
    // it, and the line's own text has not changed.
    mathline::Inside inside;
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
  void   DeleteWordBefore();
  void   DeleteWordAfter();
  void   MoveCaret(int dLine, int dChar);
  void   MoveCaretByWord(int direction);
  void   ClampCaret();
  void   TouchEdit();
  int    FunctionOfKind(int kind) const;
  void   DrawFunctionInto(Rectangle rect, int which, int shape, Vector2 centre, float span,
                          Vector3 orbit) const;
  // Where a word's worth of movement from the caret lands. One answer for the
  // arrows and for the deletes, so Ctrl+Backspace takes exactly what Ctrl+Left
  // would have skipped over.
  int    WordBoundary(int direction) const;
  // The lowest column the caret is allowed in on a line: past the sigil while a
  // text box is showing its words, and zero everywhere else.
  int    FloorCol(int line) const;
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
  Reading reading_ = Reading::Auto;
  bool   absorbed_ = false;
  bool   selected_ = false;
  std::vector<std::string> hidden_;
  bool   drawnHidden_[kDrawnKinds] = {false, false, false};
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
  // The shader that draws one of this box's functions.
  // Where the box is being read. -1 for neither, which means the end.
  mutable int                           pointerRow_ = -1;
  mutable bool                          everTouched_ = false;
  unsigned int                          traceBuffer_ = 0;
  int                                   traceFloats_ = 0;

  // The viz mesh half: a compute program, the two buffers it lays geometry
  // into, and what came back.
  unsigned int                          vizMeshProgram_ = 0;
  unsigned int                          vizTriBuffer_ = 0;
  unsigned int                          vizLineBuffer_ = 0;
  int                                   vizMeshValueLoc_ = -1;
  mutable int                           vizTriCount_ = 0;
  mutable int                           vizLineCount_ = 0;
  mutable float                         vizMeshValue_ = 0.0f;
  mutable bool                          vizMeshRun_ = false;

  // Feeding a function. Two buffers -- the spread in, the answers out -- and a
  // strip of fragments to run it, one per sample. It is a pass of its own
  // rather than a widening of the probe's, because the probe's pixels run the
  // whole body and these must not: what a function does is in its definition.
  void FeedFunction();

  unsigned int                          fnInBuffer_ = 0;
  unsigned int                          fnOutBuffer_ = 0;
  RenderTexture2D                       fnTarget_{};
  bool                                  haveFnTarget_ = false;
  int                                   fnIndexLoc_ = -1;
  int                                   fnCountLoc_ = -1;
  mutable Sampling                      sampling_;
  mutable int                           sampleHover_ = -1;
  // What was last fed through, so nothing is re-run while nothing has moved.
  std::vector<float>                    fnSignature_;

  // A sampler box's own compute program, and the buffer it lays its spread
  // into. The counter leads the buffer, exactly as a viz mesh's does.
  unsigned int                          samplerProgram_ = 0;
  unsigned int                          samplerBuffer_ = 0;
  mutable std::vector<float>            samplerRun_;
  mutable bool                          samplerRan_ = false;

  bool                                  haveFieldSurface_ = false;
  // One shader, three modes: it draws the picture, reports the values, and
  // draws the box's functions. These say which, and with what.
  int                                   modeLoc_ = -1;
  int                                   rangeLoc_ = -1;
  int                                   verticalLoc_ = -1;
  int                                   indexLoc_ = -1;
  int                                   shapeLoc_ = -1;
  int                                   thickLoc_ = -1;
  int                                   orbitLoc_ = -1;
  int                                   liftLoc_ = -1;
  int                                   probePointLoc_ = -1;
  RenderTexture2D                       probeTarget_{};
  bool                                  haveProbeTarget_ = false;
  // Where in the picture the numbers are read: under the pointer while you are
  // over it, its middle otherwise. Resolved while drawing.
  mutable Vector2                       probePoint_{0.5f, 0.5f};
  std::vector<float>                    probeSignature_;

  mutable std::vector<LineCache> cache_;
  // What the cache was last built at, so the common case -- nothing has moved
  // -- costs one comparison per line rather than a re-read of every one.
  mutable unsigned               cacheVersion_ = 0;
  mutable unsigned               cacheStyle_ = ~0u;
  mutable float                  cacheEmPx_ = -1.0f;
  mutable float                  cacheWrap_ = -1.0f;
  mutable Geometry               geometry_;
  mutable Shape                  shape_;
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
