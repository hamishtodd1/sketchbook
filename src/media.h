// Pictures and video on the canvas.
//
// Neither is mathematics, so neither lives in `types/`: an image is furniture,
// something to point at while explaining. But both are **windows** like every
// other, which is nearly the whole design. They are carried by the rim, they
// land on the grid, they are caught by the lasso, they are thrown away on the
// drop target, and the wheel resizes them the way it resizes a box.
//
// Two things are theirs alone:
//
// **A corner crops.** Dragging a corner handle moves that corner of the frame
// and leaves the picture where it is, so what changes is how much of it you can
// see -- not how big it is. Resizing is the wheel's job and cropping is the
// corner's, and keeping them apart is what makes either one predictable.
//
// **A video has a transport.** A play button that becomes a pause button, a bar
// showing where it has got to, and two markers saying where it should start and
// stop. Each marker carries the exact time as a string you can double-click and
// type into, because "about there" is not good enough when the point of the
// clip is one particular second of it.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "fontstore.h"
#include "raylib.h"
#include "video.h"
#include "viewport.h"

namespace sb {

enum class MediaKind : int { Image = 0, Video = 1 };

// True if this path looks like something we can show, and which of the two it
// is. Decided on the extension: opening the file to find out would mean opening
// every file that was ever dropped.
bool ClassifyMedia(const std::string& path, MediaKind* kind);

struct MediaItem {
  MediaKind   kind = MediaKind::Image;
  std::string path;

  Vector2 worldPos{};             // top-left
  float   worldWidth = 360.0f;    // the height follows from the crop's shape

  // The visible part of the source, in its own normalised coordinates. Cropping
  // moves these; it never moves the pixels.
  Rectangle crop{0.0f, 0.0f, 1.0f, 1.0f};

  bool    selected = false;
  bool    absorbed = false;
  Vector2 dragFrom{};

  // --- video ---------------------------------------------------------------
  bool   playing = false;
  double time = 0.0;        // where the picture has got to, seconds
  double trimStart = 0.0;
  double trimEnd = 0.0;     // seconds; the file's own end until moved

  // --- what it costs -------------------------------------------------------
  Texture2D texture{};
  bool      haveTexture = false;
  int       srcWidth = 1;
  int       srcHeight = 1;
  // Nothing at all for an image; a decoder and its child process for a video.
  std::unique_ptr<video::Stream> stream;
  video::Info                    info;
  bool                           broken = false;   // nothing would open it
};

class MediaPanel {
 public:
  // Opens `path` and puts a window for it at `worldTopLeft`. `worldWidth` of 0
  // asks for a sensible default size. Returns false if the file is not one we
  // can show at all.
  bool Add(const std::string& path, Vector2 worldTopLeft, float worldWidth = 0.0f);

  // Returns true if it took the pointer, the wheel or the keyboard.
  bool HandleInput(const Viewport& viewport);
  // Pulls the next frame of every playing video. Separate from HandleInput
  // because it happens whether or not the canvas has the pointer.
  void Advance(float seconds);
  void Draw(const Viewport& viewport, FontStore& fonts) const;

  // --- resizing ------------------------------------------------------------
  // The wheel-resize gesture belongs to the app, since it has to see every kind
  // of window at once to snap one to another. These are what it needs.
  int       ItemAt(Vector2 screenPoint, const Viewport& viewport) const;
  Rectangle WorldBounds(int index) const;
  void      Scale(int index, float factor, Vector2 anchorWorld);
  void      WorldRects(std::vector<Rectangle>* out) const;

  // True while something is being carried, cropped, scrubbed or typed into. A
  // gesture in flight is offered the pointer before anything else, or letting
  // go over another window's rim would leave this one stuck to the cursor.
  bool Busy() const { return drag_ != Drag::None || editing_ >= 0; }
  // True while a marker's time is being typed into, so Escape cancels the edit
  // rather than opening the canvas menu behind it.
  bool Editing() const { return editing_ >= 0; }
  // True only while a window is actually being carried, which is what brings up
  // the drop target.
  bool Carrying() const { return drag_ == Drag::Move && dragMoved_; }

  // --- selection -----------------------------------------------------------
  void SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport);
  // Where each of its windows is on screen.
  void Rects(const Viewport& viewport, std::vector<Rectangle>* out) const;
  void ClearSelection();
  bool AnySelected() const;
  bool SelectedRimAt(Vector2 point, const Viewport& viewport) const;
  void BeginSelectionDrag();
  void MoveSelection(Vector2 worldDelta);
  void SetSelectionAbsorbed(bool absorbed);
  void DeleteSelection();
  void DrawSelectionRims(const Viewport& viewport) const;

  // --- persistence ---------------------------------------------------------
  const std::vector<std::unique_ptr<MediaItem>>& Items() const { return items_; }
  // Textures cannot be handed back by a destructor, which may well run after
  // the window has gone. Whoever throws these away releases them first.
  void ReleaseGpu();
  void Clear();

 private:
  enum class Drag { None, Move, Crop, Scrub, Marker };

  // Everything the transport of a video occupies, in screen space.
  struct Transport {
    Rectangle strip{};
    Rectangle button{};
    Rectangle bar{};
    Rectangle startLabel{};
    Rectangle endLabel{};
    float     startX = 0.0f;
    float     endX = 0.0f;
  };

  Rectangle ScreenRect(const MediaItem& item, const Viewport& viewport) const;
  static float    AspectOf(const MediaItem& item);
  static Transport TransportOf(Rectangle rect, const MediaItem& item);
  // Which corner of `rect` the point is on, 0..3 (TL, TR, BL, BR), or -1.
  // `footer` is how much of the bottom belongs to something else -- a video's
  // transport -- and so is not the picture's corner.
  static int       CropCornerAt(Rectangle rect, Vector2 point, float footer);
  static Rectangle CornerHandle(Rectangle rect, int corner, float footer);
  static float     FooterOf(Rectangle rect, const MediaItem& item);

  void Remove(std::size_t index);
  void CommitEdit();

  std::vector<std::unique_ptr<MediaItem>> items_;

  Drag        drag_ = Drag::None;
  std::size_t dragIndex_ = 0;
  Vector2     dragGrab_{};        // pointer offset within the window, world units
  bool        dragMoved_ = false;
  int         dragCorner_ = -1;
  int         dragMarker_ = -1;   // 0 = start, 1 = end
  // A video pauses while it is scrubbed and picks up again afterwards if it was
  // playing, which is what every other player does.
  bool        resumeAfterScrub_ = false;
  // When the decoder was last asked to jump. A seek restarts ffmpeg, so a drag
  // asks for one every so often rather than once per frame.
  double      lastSeekAt_ = 0.0;

  // Typing into a marker's time. -1 when nobody is.
  int         editing_ = -1;      // index into items_
  int         editingMarker_ = -1;
  std::string editBuffer_;
  // The buffer starts holding the time it is replacing, so you can see what you
  // are changing -- but the first thing typed replaces it whole, the way a
  // spreadsheet cell does. Otherwise every edit starts with six backspaces.
  bool        editFresh_ = false;
  double      lastClickTime_ = -1.0;
  int         lastClickTarget_ = -1;
};

}  // namespace sb
