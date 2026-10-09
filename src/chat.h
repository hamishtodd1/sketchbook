// Conversations with Claude, as windows on the canvas.
//
// A window like any other -- carried by its rim, put down on the grid, caught
// by the lasso, thrown away on the cross -- holding one conversation. It is a
// fixed size and scrolls inside; a corner handle changes the size and the words
// reflow. Replies are typeset as they arrive: prose in the roman face and
// mathematics, between dollar signs, by the app's own TeX. Enter sends and
// Shift+Enter starts a new line; while a reply is coming there is a stop
// button where the send button was.
//
// What a conversation *is* lives in `conversation.*`, raylib-free. This is how
// one looks and how it is typed into.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "conversation.h"
#include "fontstore.h"
#include "raylib.h"
#include "tex/tex.h"
#include "viewport.h"

namespace sb {

struct ChatWindow {
  // Which conversation this is for as long as the app is open: how an undo step
  // refers to it without copying it. Not saved.
  int     id = 0;
  Vector2 worldPos{};
  float   width = 0.0f;    // world units
  float   height = 0.0f;
  bool    selected = false;
  Vector2 dragFrom{};
  bool    absorbed = false;

  std::unique_ptr<claude::Conversation> conversation;

  // What is being written, and where the caret is in it (a byte offset).
  std::string draft;
  std::size_t caret = 0;
  // A line under the input saying why something cannot be sent yet.
  std::string notice;

  // How far down the messages are scrolled, in world units, and whether the
  // view is following the end as a reply grows.
  float scroll = 0.0f;
  bool  following = true;

  // Each turn laid out once, in world units, and again only when its text or
  // the window's width has changed. A piece is a run of words in one face, a
  // formula, or a box drawn behind something.
  struct Piece {
    enum class Kind { Text, Math, Box, Dot };
    Kind        kind = Kind::Text;
    Face        face = Face::MainRegular;
    std::string text;
    bool        display = false;
    float       x = 0.0f;     // from the left of the messages
    float       y = 0.0f;     // the baseline, or a box's top
    float       w = 0.0f;
    float       h = 0.0f;     // a box's height
    Color       tint{};
  };
  struct Laid {
    std::vector<Piece> pieces;
    float              height = 0.0f;
    std::size_t        textSize = 0;
    bool               failed = false;
    std::string        note;
    // Laid out while still arriving, with any formula not yet closed held back.
    bool               partial = false;
  };
  std::vector<Laid> laid;
  float             laidWidth = -1.0f;

  // How far down beside the window the next box Claude makes goes, in world
  // units from the window's top.
  float madeBelow = 0.0f;
  // The display window Hamish's pointer was on when he last sent a message:
  // the one thing a look may show.
  bool        lookValid = false;
  Rectangle   lookWorld{};
  std::string lookKind;
  // The model menu, open under the header.
  bool  menuOpen = false;
};

class ChatPanel {
 public:
  ~ChatPanel();

  // A new conversation, its middle at a point on the canvas, with the caret in
  // it ready to type.
  void New(Vector2 worldCentre, float zoom);
  // How big a new conversation is, in world units.
  static Vector2 DefaultSize();
  // Says something in the newest conversation, as though typed and sent.
  void Say(const std::string& text, const std::string& effort);
  // Dev: `menu`, `rewind:N`, `rewind-canvas:N` or `say:words`, done to the first
  // conversation, so these can be tried with nobody at the pointer.
  void Dev(const std::string& action);

  // The pointer: moving and resizing windows, the wheel, the effort slider, the
  // send and stop buttons, putting the caret in the input. True when it took
  // the pointer. A press anywhere else takes the caret out.
  bool HandleInput(const Viewport& viewport, FontStore& fonts);
  // The keyboard, while a conversation has the caret.
  void HandleKeys(FontStore& fonts);
  // Takes whatever replies have arrived. Once a frame.
  void Update();
  void Draw(const Viewport& viewport, FontStore& fonts);

  bool Editing() const { return editing_ >= 0; }
  void Blur() { editing_ = -1; }
  // A gesture of this panel's own is in flight.
  bool Busy() const { return drag_ != Drag::None; }
  bool Carrying() const { return drag_ == Drag::Move && dragMoved_; }
  // Which window is under a screen point, or -1.
  int  ItemAt(Vector2 point, const Viewport& viewport) const;

  // --- selection ----------------------------------------------------------
  // The shared bodies in selection.h. Closing a conversation stops its reply.
  void SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport);
  void Rects(const Viewport& viewport, std::vector<Rectangle>* out) const;
  void ClearSelection();
  bool AnySelected() const;
  bool SelectedRimAt(Vector2 point, const Viewport& viewport) const;
  void BeginSelectionDrag();
  void MoveSelection(Vector2 worldDelta);
  void SetSelectionAbsorbed(bool absorbed);
  void DeleteSelection();
  void DrawSelectionRims(const Viewport& viewport) const;

  // Every conversation goes, and every reply in flight is stopped.
  void Clear();

  // The windows, so the app can carry out the tools their conversations ask for.
  const std::vector<std::unique_ptr<ChatWindow>>& Windows() const { return windows_; }
  // What the canvas looks like, sent along with every message (as much of it as
  // Claude has not already seen).
  void SetCanvasViewer(std::function<claude::CanvasView()> view) { view_ = std::move(view); }
  // Which display window is under the pointer, if any: noted with every message,
  // since that is the one window a look may show.
  void SetPointedWindow(std::function<bool(Rectangle*, std::string*)> pointed) {
    pointed_ = std::move(pointed);
  }
  // The canvas as a canvas file would keep it, taken with every message, so
  // rewinding to that message can put it back.
  void SetCanvasSnapshotter(std::function<std::string()> snapshot) {
    snapshot_ = std::move(snapshot);
  }
  // A rewind that asked for the canvas back: the canvas to restore, once.
  bool TakeCanvasRewind(std::string* canvas);
  // Something worth saving changed since the last time this was asked: a
  // message, a reply, a window moved, a model chosen. Conversations are not in
  // the undo steps -- a long one would make every step large -- so this is what
  // tells the canvas to save itself.
  bool TakeChanged() {
    const bool was = changed_;
    changed_ = false;
    return was;
  }
  // As the canvas file keeps them, one each, and back.
  std::vector<json::Value> Save() const;
  void                     Adopt(const std::vector<json::Value>& saved);

  // **An undo step refers to conversations rather than copying them**: which
  // windows there are, where, and how big. A conversation thrown away is set
  // aside rather than destroyed, so an undo step that has it brings it back as
  // it was -- and the file, which holds only the open ones, never keeps it.
  struct Ref {
    int     id = 0;
    Vector2 worldPos{};
    float   width = 0.0f;
    float   height = 0.0f;
    float   madeBelow = 0.0f;
  };
  std::vector<Ref> Refs() const;
  void             Restore(const std::vector<Ref>& refs);

 private:
  enum class Drag { None, Move, Resize, Effort };

  // Where the parts of a window are on screen.
  struct Parts {
    Rectangle window{};
    Rectangle header{};
    Rectangle messages{};
    Rectangle input{};
    Rectangle send{};
    Rectangle effort{};     // the slider's track, with room round it
    Rectangle corner{};     // the resize handle
    Rectangle model{};      // the model's name, which opens the menu
    Rectangle bar{};        // how full the context is
    float     menuRow = 0.0f;  // the height of a row of the model menu, on screen
    float     zoom = 1.0f;
  };
  Parts PartsOf(const ChatWindow& window, const Viewport& viewport, FontStore& fonts) const;
  float InputHeight(const ChatWindow& window, FontStore& fonts) const;

  void Layout(ChatWindow& window, FontStore& fonts);
  // Where a formula is drawn at this size, typeset the first time it is asked.
  const tex::Layout& MathAt(const std::string& source, bool display, float emPx,
                            FontStore& fonts);
  // A formula's size in world units, for laying out.
  void MathSize(const std::string& source, bool display, FontStore& fonts, float* w, float* h,
                float* d);

  void Send(ChatWindow& window);
  void Remove(std::size_t index);
  void SetEffortAt(ChatWindow& window, const Parts& parts, float x);
  // A row of the model menu, on screen.
  Rectangle MenuRow(const Parts& parts, std::size_t row) const;
  void      Rewind(std::size_t window, std::size_t turn, bool canvas);
  void PlaceCaret(ChatWindow& window, const Parts& parts, Vector2 point, FontStore& fonts);
  void DrawWindow(std::size_t index, const Viewport& viewport, FontStore& fonts, bool hovered);

  std::vector<std::unique_ptr<ChatWindow>> windows_;
  // Thrown away, and kept only so an undo can bring them back. Bounded: the
  // oldest goes first.
  std::vector<std::unique_ptr<ChatWindow>> setAside_;
  int                                      nextId_ = 1;
  int                                      editing_ = -1;
  std::function<claude::CanvasView()>      view_;
  std::function<bool(Rectangle*, std::string*)> pointed_;
  std::function<std::string()>             snapshot_;
  bool                                     changed_ = false;
  std::string                              canvasRewind_;
  bool                                     canvasRewindDue_ = false;

  // Rewinding to one of your messages. Pointing at it offers the button; the
  // button asks whether to take the canvas back too. Where these are is worked
  // out as they are drawn and pressed on the next frame.
  struct RewindOffer {
    int         window = -1;
    std::size_t turn = 0;
    bool        asking = false;
    Rectangle   chip{};
    Rectangle   words{};
    Rectangle   canvas{};
  };
  RewindOffer rewind_;

  Drag        drag_ = Drag::None;
  std::size_t dragIndex_ = 0;
  Vector2     dragGrab_{};
  bool        dragMoved_ = false;

  // Formulas typeset at the size they are being drawn at. Thrown away when the
  // size changes, and bounded, since a conversation can say a great deal.
  std::unordered_map<std::string, tex::Layout> math_;
  float                                        mathEm_ = -1.0f;
  struct Size {
    float w = 0.0f, h = 0.0f, d = 0.0f;
  };
  std::unordered_map<std::string, Size> sizes_;
};

}  // namespace sb
