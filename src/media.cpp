#include "media.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "grid.h"
#include "selection.h"
#include "text.h"
#include "hover.h"
#include "theme.h"

namespace sb {
namespace {

// A dropped picture arrives about this big, whichever way round it is.
constexpr float kDefaultSpan = 420.0f;

// Videos are decoded at preview size. Nothing here is shown at 4K, and the pipe
// would be the bottleneck if they were.
constexpr int kVideoPreviewWidth = 720;

// A crop handle: a square just inside the corner, clear of the rim's grab band
// so the two gestures never compete for the same pixel.
constexpr float kHandleInset = kRimGrabInside;
constexpr float kHandleSide = 20.0f;   // screen pixels, like every other bit of chrome

// The frame may never be cropped away to nothing.
constexpr float kMinCrop = 0.04f;

// Two clicks this close together on the same time are one double-click.
constexpr double kDoubleClickSeconds = 0.38;

std::string Lowered(const std::string& value) {
  std::string out = value;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string Extension(const std::string& path) {
  const std::size_t dot = path.find_last_of('.');
  const std::size_t slash = path.find_last_of("/\\");
  if (dot == std::string::npos) return {};
  if (slash != std::string::npos && dot < slash) return {};
  return Lowered(path.substr(dot));
}

// m:ss.s -- long enough to be exact, short enough to sit under a marker.
std::string FormatTime(double seconds) {
  if (!(seconds > 0.0)) seconds = 0.0;
  const int minutes = static_cast<int>(seconds) / 60;
  const double rest = seconds - minutes * 60.0;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%d:%04.1f", minutes, rest);
  return buffer;
}

// Accepts what FormatTime writes, and also a bare number of seconds -- typing
// "12" should mean twelve seconds, not twelve minutes.
double ParseTime(const std::string& text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos) return std::atof(text.c_str());
  const double minutes = std::atof(text.substr(0, colon).c_str());
  const double seconds = std::atof(text.substr(colon + 1).c_str());
  return minutes * 60.0 + seconds;
}

void DrawCentred(FontStore& fonts, const std::string& label, Rectangle box, float em, Color tint) {
  const float width = text::Measure(fonts, Face::MainRegular, label, em);
  text::Draw(fonts, Face::MainRegular, label,
             {box.x + (box.width - width) * 0.5f, box.y + box.height * 0.5f + em * 0.35f}, em,
             tint);
}

}  // namespace

bool ClassifyMedia(const std::string& path, MediaKind* kind) {
  const std::string ext = Extension(path);
  // raylib's own image loaders, minus the ones that need a flag turned on.
  static const char* kImages[] = {".png", ".jpg", ".jpeg", ".bmp", ".tga",
                                  ".gif", ".qoi", ".psd",  ".hdr", ".pic"};
  static const char* kVideos[] = {".mp4", ".mov", ".mkv", ".avi", ".webm",
                                  ".m4v", ".wmv", ".mpg", ".mpeg", ".gifv"};
  for (const char* candidate : kImages) {
    if (ext == candidate) {
      if (kind != nullptr) *kind = MediaKind::Image;
      return true;
    }
  }
  for (const char* candidate : kVideos) {
    if (ext == candidate) {
      if (kind != nullptr) *kind = MediaKind::Video;
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------- adding

bool MediaPanel::Add(const std::string& path, Vector2 worldTopLeft, float worldWidth) {
  MediaKind kind = MediaKind::Image;
  if (!ClassifyMedia(path, &kind)) return false;

  auto item = std::make_unique<MediaItem>();
  item->kind = kind;
  item->path = path;
  item->worldPos = worldTopLeft;

  if (kind == MediaKind::Image) {
    Image image = LoadImage(path.c_str());
    if (image.data == nullptr) {
      item->broken = true;
    } else {
      item->srcWidth = image.width;
      item->srcHeight = image.height;
      item->texture = LoadTextureFromImage(image);
      item->haveTexture = true;
      SetTextureFilter(item->texture, TEXTURE_FILTER_BILINEAR);
      UnloadImage(image);
    }
  } else {
    item->info = video::Probe(path);
    if (!item->info.ok) {
      item->broken = true;
    } else {
      item->stream = std::make_unique<video::Stream>(path, item->info, kVideoPreviewWidth);
      item->srcWidth = item->stream->Width();
      item->srcHeight = item->stream->Height();
      // One texture for the life of the window; every frame is written into it
      // rather than a new one being made and thrown away sixty times a second.
      Image blank = GenImageColor(item->srcWidth, item->srcHeight, BLACK);
      ImageFormat(&blank, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
      item->texture = LoadTextureFromImage(blank);
      item->haveTexture = true;
      SetTextureFilter(item->texture, TEXTURE_FILTER_BILINEAR);
      UnloadImage(blank);
      item->trimEnd = (item->info.duration > 0.05) ? item->info.duration : 60.0;
      item->playing = true;
    }
  }

  if (item->broken) {
    item->srcWidth = 16;
    item->srcHeight = 9;
  }

  if (worldWidth > 0.0f) {
    item->worldWidth = worldWidth;
  } else {
    // Arrive at a size that fits in a square of kDefaultSpan whichever way
    // round the picture is, so a tall photo does not open the height of a wall.
    const float aspect = static_cast<float>(item->srcWidth) / static_cast<float>(item->srcHeight);
    item->worldWidth = (aspect >= 1.0f) ? kDefaultSpan : kDefaultSpan * aspect;
  }

  items_.push_back(std::move(item));
  return true;
}

void MediaPanel::Remove(std::size_t index) {
  if (index >= items_.size()) return;
  MediaItem& item = *items_[index];
  if (item.haveTexture) {
    UnloadTexture(item.texture);
    item.haveTexture = false;
  }
  // The decoder goes with it, which stops its child process: a window nobody
  // can see must not still be pulling frames off a pipe.
  item.stream.reset();
  items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
  if (editing_ >= 0) {
    editing_ = -1;
    editingMarker_ = -1;
  }
  drag_ = Drag::None;
}

void MediaPanel::ReleaseGpu() {
  for (const std::unique_ptr<MediaItem>& item : items_) {
    if (item->haveTexture) {
      UnloadTexture(item->texture);
      item->haveTexture = false;
    }
    item->stream.reset();
  }
}

void MediaPanel::Clear() {
  ReleaseGpu();
  items_.clear();
  drag_ = Drag::None;
  editing_ = -1;
  editingMarker_ = -1;
}

// ----------------------------------------------------------------- geometry

float MediaPanel::AspectOf(const MediaItem& item) {
  const float w = item.crop.width * static_cast<float>(item.srcWidth);
  const float h = item.crop.height * static_cast<float>(item.srcHeight);
  if (!(w > 0.0f) || !(h > 0.0f)) return 1.0f;
  return w / h;
}

Rectangle MediaPanel::ScreenRect(const MediaItem& item, const Viewport& viewport) const {
  const float   zoom = viewport.Zoom();
  const Vector2 topLeft = viewport.ToScreen(item.worldPos);
  const float   width = item.worldWidth * zoom;
  return {topLeft.x, topLeft.y, width, width / AspectOf(item)};
}

Rectangle MediaPanel::WorldBounds(int index) const {
  if (index < 0 || index >= static_cast<int>(items_.size())) return {};
  const MediaItem& item = *items_[static_cast<std::size_t>(index)];
  return {item.worldPos.x, item.worldPos.y, item.worldWidth,
          item.worldWidth / AspectOf(item)};
}

void MediaPanel::WorldRects(std::vector<Rectangle>* out) const {
  if (out == nullptr) return;
  for (std::size_t i = 0; i < items_.size(); ++i) out->push_back(WorldBounds(static_cast<int>(i)));
}

void MediaPanel::Scale(int index, float factor, Vector2 anchorWorld) {
  if (index < 0 || index >= static_cast<int>(items_.size())) return;
  MediaItem& item = *items_[static_cast<std::size_t>(index)];
  const float wanted = std::clamp(item.worldWidth * factor, 24.0f, 40000.0f);
  const float applied = wanted / item.worldWidth;
  item.worldPos = {anchorWorld.x + (item.worldPos.x - anchorWorld.x) * applied,
                   anchorWorld.y + (item.worldPos.y - anchorWorld.y) * applied};
  item.worldWidth = wanted;
}

Rectangle MediaPanel::CornerHandle(Rectangle rect, int corner, float footer) {
  const float side = std::min(kHandleSide, std::min(rect.width, rect.height) * 0.4f);
  const float inset = std::min(kHandleInset, std::min(rect.width, rect.height) * 0.2f);
  const float left = rect.x + inset;
  const float top = rect.y + inset;
  const float right = rect.x + rect.width - inset - side;
  // A video's transport covers the foot of the picture, so the bottom corners
  // sit above it: a handle you cannot reach is not a handle.
  const float bottom = rect.y + rect.height - footer - inset - side;
  switch (corner) {
    case 0: return {left, top, side, side};
    case 1: return {right, top, side, side};
    case 2: return {left, bottom, side, side};
    default: return {right, bottom, side, side};
  }
}

int MediaPanel::CropCornerAt(Rectangle rect, Vector2 point, float footer) {
  if (rect.width < 40.0f || rect.height - footer < 40.0f) return -1;
  for (int corner = 0; corner < 4; ++corner) {
    if (CheckCollisionPointRec(point, CornerHandle(rect, corner, footer))) return corner;
  }
  return -1;
}

float MediaPanel::FooterOf(Rectangle rect, const MediaItem& item) {
  if (item.kind != MediaKind::Video || item.broken) return 0.0f;
  return TransportOf(rect, item).strip.height;
}

MediaPanel::Transport MediaPanel::TransportOf(Rectangle rect, const MediaItem& item) {
  Transport t;
  const float height = std::min(std::clamp(rect.height * 0.17f, 20.0f, 42.0f), rect.height * 0.45f);
  t.strip = {rect.x, rect.y + rect.height - height, rect.width, height};

  const float pad = height * 0.18f;
  const float side = height - 2.0f * pad;
  t.button = {t.strip.x + pad, t.strip.y + pad, side, side};

  const float barLeft = t.button.x + t.button.width + pad * 2.0f;
  const float barRight = t.strip.x + t.strip.width - pad * 2.0f;
  t.bar = {barLeft, t.strip.y + height * 0.44f, std::max(6.0f, barRight - barLeft),
           std::max(3.0f, height * 0.14f)};

  const double duration = (item.info.duration > 0.05) ? item.info.duration : 60.0;
  const auto at = [&](double seconds) {
    const double fraction = std::clamp(seconds / duration, 0.0, 1.0);
    return t.bar.x + t.bar.width * static_cast<float>(fraction);
  };
  t.startX = at(item.trimStart);
  t.endX = at(item.trimEnd);

  // The times sit above the bar and read *outwards* from their markers: the
  // start to the left of its own, the end to the right of its own. They cannot
  // collide however short the clip is, which they would if they read inwards.
  const float labelH = std::clamp(height * 0.62f, 13.0f, 22.0f);
  const float labelW = labelH * 2.9f;
  const float labelY = t.strip.y - labelH - 3.0f;
  const float leftLimit = rect.x + 2.0f;
  const float rightLimit = rect.x + rect.width - labelW - 2.0f;
  t.startLabel = {std::clamp(t.startX - labelW, leftLimit, rightLimit), labelY, labelW, labelH};
  t.endLabel = {std::clamp(t.endX, leftLimit, rightLimit), labelY, labelW, labelH};
  return t;
}

int MediaPanel::ItemAt(Vector2 screenPoint, const Viewport& viewport) const {
  for (int i = static_cast<int>(items_.size()) - 1; i >= 0; --i) {
    if (CheckCollisionPointRec(screenPoint, ScreenRect(*items_[static_cast<std::size_t>(i)],
                                                       viewport))) {
      return i;
    }
  }
  return -1;
}

// -------------------------------------------------------------------- input

void MediaPanel::CommitEdit() {
  if (editing_ < 0 || editing_ >= static_cast<int>(items_.size())) {
    editing_ = -1;
    editingMarker_ = -1;
    return;
  }
  MediaItem& item = *items_[static_cast<std::size_t>(editing_)];
  const double duration = (item.info.duration > 0.05) ? item.info.duration : 60.0;
  const double value = std::clamp(ParseTime(editBuffer_), 0.0, duration);
  if (editingMarker_ == 0) {
    item.trimStart = std::min(value, item.trimEnd - 0.05);
    if (item.time < item.trimStart) {
      item.time = item.trimStart;
      if (item.stream) item.stream->Seek(item.time);
    }
  } else {
    item.trimEnd = std::max(value, item.trimStart + 0.05);
  }
  editing_ = -1;
  editingMarker_ = -1;
}

bool MediaPanel::HandleInput(const Viewport& viewport) {
  const Vector2 pointer = GetMousePosition();
  const float   zoom = std::max(viewport.Zoom(), 1e-6f);

  // --- typing a time -------------------------------------------------------
  if (editing_ >= 0) {
    for (int c = GetCharPressed(); c > 0; c = GetCharPressed()) {
      if ((c >= '0' && c <= '9') || c == ':' || c == '.') {
        if (editFresh_) {
          editBuffer_.clear();
          editFresh_ = false;
        }
        if (editBuffer_.size() < 12) editBuffer_ += static_cast<char>(c);
      }
    }
    if (IsKeyPressed(KEY_BACKSPACE) && !editBuffer_.empty()) {
      editFresh_ = false;
      editBuffer_.pop_back();
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
      CommitEdit();
    } else if (IsKeyPressed(KEY_ESCAPE)) {
      editing_ = -1;
      editingMarker_ = -1;
    } else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      // Clicking away is agreeing with what you typed, as it is everywhere else.
      CommitEdit();
    }
    return true;
  }

  // --- a gesture already under way ----------------------------------------
  if (drag_ != Drag::None && dragIndex_ < items_.size()) {
    MediaItem& item = *items_[dragIndex_];
    const Rectangle rect = ScreenRect(item, viewport);
    const Vector2   delta = GetMouseDelta();

    switch (drag_) {
      case Drag::Move: {
        if (delta.x != 0.0f || delta.y != 0.0f) dragMoved_ = true;
        const bool absorbing = dragMoved_ && OverDropTarget(pointer);
        item.absorbed = absorbing;
        if (!absorbing) {
          const Vector2 world = viewport.ToWorld(pointer);
          item.worldPos = SnapToGrid({world.x - dragGrab_.x, world.y - dragGrab_.y}, zoom);
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
          item.absorbed = false;
          drag_ = Drag::None;
          dragMoved_ = false;
          if (absorbing) Remove(dragIndex_);
        }
        break;
      }
      case Drag::Crop: {
        const float su = item.crop.width / std::max(rect.width, 1.0f);
        const float sv = item.crop.height / std::max(rect.height, 1.0f);
        const bool  left = (dragCorner_ == 0 || dragCorner_ == 2);
        const bool  top = (dragCorner_ == 0 || dragCorner_ == 1);

        if (left) {
          const float du = std::clamp(delta.x * su, -item.crop.x, item.crop.width - kMinCrop);
          item.crop.x += du;
          item.crop.width -= du;
          item.worldPos.x += (du / su) / zoom;
          item.worldWidth -= (du / su) / zoom;
        } else {
          const float du = std::clamp(delta.x * su, kMinCrop - item.crop.width,
                                      1.0f - item.crop.x - item.crop.width);
          item.crop.width += du;
          item.worldWidth += (du / su) / zoom;
        }
        if (top) {
          const float dv = std::clamp(delta.y * sv, -item.crop.y, item.crop.height - kMinCrop);
          item.crop.y += dv;
          item.crop.height -= dv;
          item.worldPos.y += (dv / sv) / zoom;
        } else {
          const float dv = std::clamp(delta.y * sv, kMinCrop - item.crop.height,
                                      1.0f - item.crop.y - item.crop.height);
          item.crop.height += dv;
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) drag_ = Drag::None;
        break;
      }
      case Drag::Scrub:
      case Drag::Marker: {
        const Transport t = TransportOf(rect, item);
        const double duration = (item.info.duration > 0.05) ? item.info.duration : 60.0;
        const double at = std::clamp<double>((pointer.x - t.bar.x) / std::max(t.bar.width, 1.0f),
                                             0.0, 1.0) *
                          duration;
        if (drag_ == Drag::Scrub) {
          item.time = at;
          // Seeking means restarting the decoder, so a drag asks for one every
          // so often rather than sixty times a second.
          if (item.stream && GetTime() - lastSeekAt_ > 0.1) {
            item.stream->Seek(item.time);
            lastSeekAt_ = GetTime();
          }
        } else if (dragMarker_ == 0) {
          item.trimStart = std::min(at, item.trimEnd - 0.05);
        } else {
          item.trimEnd = std::max(at, item.trimStart + 0.05);
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
          if (drag_ == Drag::Scrub) {
            // The last seek is not throttled: wherever the pointer was let go
            // is exactly where the picture has to end up.
            if (item.stream) item.stream->Seek(item.time);
            if (resumeAfterScrub_) item.playing = true;
          }
          drag_ = Drag::None;
        }
        break;
      }
      default:
        drag_ = Drag::None;
        break;
    }
    return true;
  }

  // --- starting one --------------------------------------------------------
  for (int i = static_cast<int>(items_.size()) - 1; i >= 0; --i) {
    const std::size_t index = static_cast<std::size_t>(i);
    MediaItem&        item = *items_[index];
    const Rectangle   rect = ScreenRect(item, viewport);

    const bool inside = CheckCollisionPointRec(pointer, rect);
    const bool onRim = PointOnRim(rect, pointer);
    if (!inside && !onRim) continue;

    // The transport comes first: it sits over the picture, so a click there is
    // never a crop and never a carry.
    if (item.kind == MediaKind::Video && !item.broken) {
      const Transport t = TransportOf(rect, item);
      const bool      onLabel = CheckCollisionPointRec(pointer, t.startLabel) ||
                           CheckCollisionPointRec(pointer, t.endLabel);
      if (CheckCollisionPointRec(pointer, t.strip) || onLabel) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
          const bool onStart = CheckCollisionPointRec(pointer, t.startLabel);
          const bool onEnd = CheckCollisionPointRec(pointer, t.endLabel);
          const int  target = onStart ? (i * 2) : (onEnd ? (i * 2 + 1) : -1);
          const bool doubled = target >= 0 && target == lastClickTarget_ &&
                               GetTime() - lastClickTime_ < kDoubleClickSeconds;
          lastClickTime_ = GetTime();
          lastClickTarget_ = target;

          if (doubled) {
            // A time you can type is the only way to be exact about where a
            // clip begins, and that is usually the whole point of trimming it.
            editing_ = i;
            editingMarker_ = onStart ? 0 : 1;
            editBuffer_ = FormatTime(onStart ? item.trimStart : item.trimEnd);
            editFresh_ = true;
          } else if (CheckCollisionPointRec(pointer, t.button)) {
            item.playing = !item.playing;
            if (item.playing && item.time >= item.trimEnd - 0.02) {
              item.time = item.trimStart;
              if (item.stream) item.stream->Seek(item.time);
            }
          } else if (std::abs(pointer.x - t.startX) < 9.0f && !onEnd) {
            drag_ = Drag::Marker;
            dragMarker_ = 0;
            dragIndex_ = index;
          } else if (std::abs(pointer.x - t.endX) < 9.0f && !onStart) {
            drag_ = Drag::Marker;
            dragMarker_ = 1;
            dragIndex_ = index;
          } else if (CheckCollisionPointRec(pointer, {t.bar.x, t.strip.y, t.bar.width,
                                                      t.strip.height})) {
            drag_ = Drag::Scrub;
            dragIndex_ = index;
            resumeAfterScrub_ = item.playing;
            item.playing = false;
            lastSeekAt_ = 0.0;
          }
        }
        return true;
      }
    }

    if (inside) {
      const int corner = CropCornerAt(rect, pointer, FooterOf(rect, item));
      if (corner >= 0) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
          drag_ = Drag::Crop;
          dragIndex_ = index;
          dragCorner_ = corner;
        }
        return true;
      }
    }

    if (onRim) {
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const Vector2 world = viewport.ToWorld(pointer);
        dragGrab_ = {world.x - item.worldPos.x, world.y - item.worldPos.y};
        drag_ = Drag::Move;
        dragIndex_ = index;
        dragMoved_ = false;
      }
      return true;   // hovering a rim claims the pointer, as everywhere else
    }

    // Inside a picture with nothing else to do: it still takes the pointer, so
    // a click there does not lasso the canvas out from under it.
    return inside;
  }
  return false;
}

void MediaPanel::Advance(float seconds) {
  for (const std::unique_ptr<MediaItem>& item : items_) {
    if (item->kind != MediaKind::Video || !item->stream) continue;

    if (item->playing) {
      item->time += seconds;
      if (item->time >= item->trimEnd || item->stream->Ended()) {
        // Loops between the markers: a clip chosen for one moment should keep
        // showing that moment rather than stopping on a black frame.
        item->time = item->trimStart;
        item->stream->Seek(item->time);
      }
    }
    if (item->stream->Advance(item->time) && item->haveTexture) {
      UpdateTexture(item->texture, item->stream->Pixels().data());
    }
  }
}

// ------------------------------------------------------------------ drawing

void MediaPanel::Draw(const Viewport& viewport, FontStore& fonts) const {
  const Vector2 pointer = GetMousePosition();
  const float   zoom = viewport.Zoom();

  for (std::size_t i = 0; i < items_.size(); ++i) {
    const MediaItem& item = *items_[i];
    if (item.absorbed) continue;

    const Rectangle rect = ScreenRect(item, viewport);
    if (rect.x > GetScreenWidth() || rect.y > GetScreenHeight() || rect.x + rect.width < 0.0f ||
        rect.y + rect.height < 0.0f) {
      continue;
    }

    if (item.broken || !item.haveTexture) {
      DrawRectangleRec(rect, kMediaBroken);
      const float em = std::clamp(rect.height * 0.12f, 8.0f, 22.0f);
      const char* why = (item.kind == MediaKind::Video && video::ToolPath().empty())
                            ? "no ffmpeg"
                            : "cannot open";
      DrawCentred(fonts, why, rect, em, kChromeText);
    } else {
      const Rectangle source = {item.crop.x * static_cast<float>(item.srcWidth),
                                item.crop.y * static_cast<float>(item.srcHeight),
                                item.crop.width * static_cast<float>(item.srcWidth),
                                item.crop.height * static_cast<float>(item.srcHeight)};
      DrawTexturePro(item.texture, source, rect, {0.0f, 0.0f}, 0.0f, WHITE);
    }

    // Being anywhere on the picture is enough to bring up the crop corners --
    // they have to be findable -- but the rim answers to the rim, exactly as it
    // does on every other window.
    // The picture being held is not being hovered, it is being worked on, so it
    // keeps the real pointer and its handles stay where the hand is. Every other
    // picture sees the hover pointer, which is nowhere while anything is held.
    const bool    heldHere = drag_ != Drag::None && dragIndex_ == i;
    const Vector2 here = heldHere ? pointer : hover::Pointer();
    const bool    hovered = CheckCollisionPointRec(here, rect) || PointOnRim(rect, here);
    const bool    onRim = PointOnRim(rect, here);

    // --- the transport ----------------------------------------------------
    if (item.kind == MediaKind::Video && !item.broken && rect.height > 40.0f) {
      const Transport t = TransportOf(rect, item);
      DrawRectangleRec(t.strip, kTransportBg);

      // Play, or pause: two bars, which is the same shape everywhere.
      if (item.playing) {
        const float w = t.button.width * 0.28f;
        DrawRectangleRec({t.button.x + t.button.width * 0.14f, t.button.y, w, t.button.height},
                         kTransportFg);
        DrawRectangleRec({t.button.x + t.button.width * 0.58f, t.button.y, w, t.button.height},
                         kTransportFg);
      } else {
        DrawTriangle({t.button.x + t.button.width * 0.18f, t.button.y},
                     {t.button.x + t.button.width * 0.18f, t.button.y + t.button.height},
                     {t.button.x + t.button.width * 0.92f,
                      t.button.y + t.button.height * 0.5f},
                     kTransportFg);
      }

      // The whole file is the dim track; the chosen clip is the lit part of it,
      // and the playhead is a dot rather than a second fill -- one bar saying
      // two things at once says neither.
      DrawRectangleRec(t.bar, kBarTrack);
      DrawRectangleRec({t.startX, t.bar.y, std::max(1.0f, t.endX - t.startX), t.bar.height},
                       kBarClip);
      const double duration = (item.info.duration > 0.05) ? item.info.duration : 60.0;
      const float  head = t.bar.x + t.bar.width * static_cast<float>(
                                                     std::clamp(item.time / duration, 0.0, 1.0));
      DrawCircleV({head, t.bar.y + t.bar.height * 0.5f}, t.bar.height * 1.6f, kBarPlayed);

      // Each marker points into the clip, so which end of it you have hold of
      // is legible without reading the times.
      const float markH = t.strip.height * 0.42f;
      for (int m = 0; m < 2; ++m) {
        const float x = (m == 0) ? t.startX : t.endX;
        const float tip = (m == 0) ? (x + markH * 0.5f) : (x - markH * 0.5f);
        const float back = (m == 0) ? (x - markH * 0.5f) : (x + markH * 0.5f);
        // Wound counter-clockwise either way round: raylib culls a triangle
        // given the other way, and the mirrored marker would simply not appear.
        if (m == 0) {
          DrawTriangle({back, t.bar.y - markH}, {back, t.bar.y}, {tip, t.bar.y - markH * 0.5f},
                       kTrimMark);
        } else {
          DrawTriangle({back, t.bar.y}, {back, t.bar.y - markH}, {tip, t.bar.y - markH * 0.5f},
                       kTrimMark);
        }
        DrawRectangleRec({x - 1.0f, t.bar.y - markH, 2.0f, markH + t.bar.height}, kTrimMark);
      }

      // The exact times, and the one being typed into.
      const float em = std::clamp(t.startLabel.height * 0.72f, 7.0f, 16.0f);
      if (em >= 7.0f) {
        for (int m = 0; m < 2; ++m) {
          const Rectangle box = (m == 0) ? t.startLabel : t.endLabel;
          const bool      active = (editing_ == static_cast<int>(i) && editingMarker_ == m);
          const std::string label =
              active ? editBuffer_ + "|" : FormatTime(m == 0 ? item.trimStart : item.trimEnd);
          DrawRectangleRounded(box, 0.35f, 6, active ? kAccent : kTransportBg);
          DrawCentred(fonts, label, box, em, kTransportFg);
        }
      }
    }

    // --- the crop corners --------------------------------------------------
    const float footer = FooterOf(rect, item);
    if ((hovered || item.selected) && !item.broken && rect.width > 40.0f &&
        rect.height - footer > 40.0f) {
      for (int corner = 0; corner < 4; ++corner) {
        const Rectangle handle = CornerHandle(rect, corner, footer);
        const bool      hot = CheckCollisionPointRec(here, handle);
        // An L-bracket rather than a square: it reads as a corner of a frame,
        // which is what dragging it moves.
        const float thickness = std::max(2.0f, handle.width * 0.16f);
        const bool  left = (corner == 0 || corner == 2);
        const bool  top = (corner == 0 || corner == 1);
        const Color tint = hot ? kCropHandleHot : kCropHandle;
        DrawRectangleRec({handle.x, top ? handle.y : handle.y + handle.height - thickness,
                          handle.width, thickness},
                         tint);
        DrawRectangleRec({left ? handle.x : handle.x + handle.width - thickness, handle.y,
                          thickness, handle.height},
                         tint);
      }
    }

    if (onRim || item.selected) DrawRim(rect, zoom, item.selected);
  }
}

// ---------------------------------------------------------------- selection

void MediaPanel::Rects(const Viewport& viewport, std::vector<Rectangle>* out) const {
  AppendRects(items_, out,
              [&](std::size_t i) { return ScreenRect(*items_[i], viewport); });
}

void MediaPanel::SelectWithin(const std::vector<Vector2>& lasso, const Viewport& viewport) {
  SelectCaught(items_, lasso,
               [&](std::size_t i) { return ScreenRect(*items_[i], viewport); });
}

void MediaPanel::ClearSelection() { ClearSelected(items_); }

bool MediaPanel::AnySelected() const { return sb::AnySelected(items_); }

bool MediaPanel::SelectedRimAt(Vector2 point, const Viewport& viewport) const {
  return SelectedRim(items_, point,
                     [&](std::size_t i) { return ScreenRect(*items_[i], viewport); });
}

void MediaPanel::BeginSelectionDrag() { RememberPositions(items_); }

void MediaPanel::MoveSelection(Vector2 worldDelta) { MoveSelected(items_, worldDelta); }

void MediaPanel::SetSelectionAbsorbed(bool absorbed) {
  for (const std::unique_ptr<MediaItem>& item : items_) {
    if (item->selected) item->absorbed = absorbed;
  }
}

void MediaPanel::DeleteSelection() {
  for (std::size_t i = items_.size(); i-- > 0;) {
    if (items_[i]->selected) Remove(i);
  }
}

void MediaPanel::DrawSelectionRims(const Viewport& viewport) const {
  for (const std::unique_ptr<MediaItem>& item : items_) {
    if (!item->selected || item->absorbed) continue;
    DrawRim(ScreenRect(*item, viewport), viewport.Zoom(), true);
  }
}

}  // namespace sb
