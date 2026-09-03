#include "slides.h"

#include <algorithm>
#include <cmath>

#include "text.h"
#include "theme.h"

namespace sb {
namespace {

// The button, in world units.
constexpr float kButtonWidth = 176.0f;
constexpr float kButtonHeight = 52.0f;
constexpr float kButtonEm = 22.0f;
constexpr float kButtonSeconds = 0.55f;

// How long the camera takes to arrive at a slide.
constexpr float kGlideSeconds = 0.6f;

// The button that throws a slide away. It sits in the slide's bottom-right
// corner, just inside the rim's grab band so the two never want the same pixel,
// and it says what it does. Chrome, so it is a fixed number of screen pixels
// however far the canvas is zoomed -- and it is simply not offered on a slide
// too small on screen to hold it.
constexpr const char* kDeleteLabel = "delete slide";
constexpr float kDeleteEm = 15.0f;
constexpr float kDeletePad = 11.0f;
constexpr float kDeleteHeight = 27.0f;

// Padding inside a slide, as a fraction of its height, and the size text is set
// at. Both in the slide's own terms, so they hold at any zoom.
constexpr float kPadFraction = 0.07f;
constexpr float kTextEm = 44.0f;

float Smoothstep(float t) {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  return clamped * clamped * (3.0f - 2.0f * clamped);
}

}  // namespace

void SlidePanel::Reset() {
  slides_.clear();
  buttonPos_ = DefaultButtonPosition();
  buttonMoving_ = false;
  pending_ = {};
  removal_ = {};
  hoverSlide_ = -1;
}

NewSlide SlidePanel::TakeNewSlide() {
  const NewSlide taken = pending_;
  pending_ = {};
  return taken;
}

SlideRemoval SlidePanel::TakeRemoval() {
  const SlideRemoval taken = removal_;
  removal_ = {};
  return taken;
}

Rectangle SlidePanel::OnScreen(const Slide& slide, const Viewport& viewport, float aspect) {
  const Rectangle bounds = Bounds(slide, aspect);
  const Vector2   topLeft = viewport.ToScreen({bounds.x, bounds.y});
  const float     zoom = viewport.Zoom();
  return {topLeft.x, topLeft.y, bounds.width * zoom, bounds.height * zoom};
}

Rectangle SlidePanel::DeleteButtonRect(Rectangle rim, FontStore& fonts) {
  const float width =
      text::Measure(fonts, Face::MainRegular, kDeleteLabel, kDeleteEm) + 2.0f * kDeletePad;
  // A slide too small on screen would be all button, so it gets none.
  if (rim.width < width * 2.2f || rim.height < kDeleteHeight * 3.0f) return {};
  return {rim.x + rim.width - kRimGrabInside - width,
          rim.y + rim.height - kRimGrabInside - kDeleteHeight, width, kDeleteHeight};
}

void SlidePanel::RemoveSlide(int index, float aspect) {
  if (index < 0 || index >= static_cast<int>(slides_.size())) return;
  const Slide gone = slides_[static_cast<std::size_t>(index)];
  const float shift = kSlideHeight + kSlideGap;

  removal_.asked = true;
  removal_.bounds = Bounds(gone, aspect);
  removal_.shift = shift;

  slides_.erase(slides_.begin() + index);
  // A deck is a column, so what came after closes the gap rather than leaving a
  // hole in the middle of it.
  for (Slide& slide : slides_) {
    if (slide.centre.y > gone.centre.y) slide.centre.y -= shift;
  }
  if (buttonPos_.y > gone.centre.y) {
    buttonPos_.y -= shift;
    // A glide still running would put the button straight back where it was.
    buttonMoving_ = false;
  }
  hoverSlide_ = -1;
}

float SlidePanel::TextEm() { return kTextEm; }

float SlidePanel::ContentWidth(float aspect) {
  const Slide slide{{0.0f, 0.0f}};
  return Bounds(slide, aspect).width - 2.0f * kSlideHeight * kPadFraction;
}

Vector2 SlidePanel::TitleAnchor(Vector2 centre, float aspect) {
  const Slide     slide{centre};
  const Rectangle bounds = Bounds(slide, aspect);
  const float     pad = kSlideHeight * kPadFraction;
  return {bounds.x + pad, bounds.y + pad};
}


void SlidePanel::Adopt(std::vector<Slide> slides, Vector2 buttonPosition) {
  slides_ = std::move(slides);
  buttonPos_ = buttonPosition;
  buttonMoving_ = false;
  removal_ = {};
  hoverSlide_ = -1;
}

Rectangle SlidePanel::Bounds(const Slide& slide, float aspect) {
  const float height = kSlideHeight;
  const float width = height * std::max(aspect, 0.1f);
  return {slide.centre.x - width * 0.5f, slide.centre.y - height * 0.5f, width, height};
}

float SlidePanel::ZoomToFill(const Slide& slide, float aspect) {
  const Rectangle bounds = Bounds(slide, aspect);
  const float byWidth = static_cast<float>(GetScreenWidth()) / bounds.width;
  const float byHeight = static_cast<float>(GetScreenHeight()) / bounds.height;
  // Cover rather than fit: the slide fills the window and its rim finishes just
  // outside it, which is what "presenting" looks like.
  return std::max(byWidth, byHeight) * 1.01f;
}

Rectangle SlidePanel::ButtonRect(const Viewport& viewport) const {
  const float zoom = viewport.Zoom();
  const Vector2 centre = viewport.ToScreen(buttonPos_);
  return {centre.x - kButtonWidth * 0.5f * zoom, centre.y - kButtonHeight * 0.5f * zoom,
          kButtonWidth * zoom, kButtonHeight * zoom};
}

void SlidePanel::Advance(float seconds) {
  if (!buttonMoving_) return;
  buttonElapsed_ += seconds;
  const float t = Smoothstep(buttonElapsed_ / kButtonSeconds);
  buttonPos_ = {buttonFrom_.x + (buttonTo_.x - buttonFrom_.x) * t,
                buttonFrom_.y + (buttonTo_.y - buttonFrom_.y) * t};
  if (buttonElapsed_ >= kButtonSeconds) {
    buttonPos_ = buttonTo_;
    buttonMoving_ = false;
  }
}

bool SlidePanel::HandleInput(const Viewport& viewport, FontStore& fonts, float aspect,
                             Viewport* camera) {
  const Vector2 pointer = GetMousePosition();

  const Rectangle button = ButtonRect(viewport);
  if (CheckCollisionPointRec(pointer, button)) {
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !buttonMoving_) {
      // The slide is left where the button was; the button goes on below it, so
      // the next one lands under this one and the deck reads downwards.
      Slide slide;
      slide.centre = buttonPos_;
      slides_.push_back(slide);
      // The title and the body are windows like any other, and windows belong
      // to the canvas rather than to this panel. So it says a slide has
      // appeared and lets the canvas furnish it.
      pending_ = {true, slide.centre};

      buttonFrom_ = buttonPos_;
      buttonTo_ = {buttonPos_.x, buttonPos_.y + kSlideHeight + kSlideGap};
      buttonElapsed_ = 0.0f;
      buttonMoving_ = true;

      // And the camera goes to look at what was just made, exactly as it would
      // if the slide's rim had been clicked. Making a slide is a way of saying
      // "I am working here now".
      if (camera != nullptr) {
        camera->GlideTo(slide.centre, ZoomToFill(slide, aspect), kGlideSeconds);
      }
    }
    return true;   // hovering claims it, so a click never falls through
  }

  // A slide is entered by its rim. Its inside belongs to whatever is on it.
  int onRim = -1;
  for (std::size_t i = 0; i < slides_.size(); ++i) {
    if (PointOnRim(OnScreen(slides_[i], viewport, aspect), pointer)) {
      onRim = static_cast<int>(i);
      break;
    }
  }

  // Pointing at a slide's rim offers its delete button, and going to the button
  // keeps the offer open -- otherwise reaching for it would take it away.
  int showing = onRim;
  if (showing < 0 && hoverSlide_ >= 0 && hoverSlide_ < static_cast<int>(slides_.size())) {
    const Rectangle rim =
        OnScreen(slides_[static_cast<std::size_t>(hoverSlide_)], viewport, aspect);
    if (CheckCollisionPointRec(pointer, DeleteButtonRect(rim, fonts))) showing = hoverSlide_;
  }
  hoverSlide_ = showing;

  if (hoverSlide_ >= 0) {
    const Rectangle rim =
        OnScreen(slides_[static_cast<std::size_t>(hoverSlide_)], viewport, aspect);
    if (CheckCollisionPointRec(pointer, DeleteButtonRect(rim, fonts))) {
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) RemoveSlide(hoverSlide_, aspect);
      return true;
    }
  }

  if (onRim >= 0) {
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && camera != nullptr) {
      const Slide& slide = slides_[static_cast<std::size_t>(onRim)];
      camera->GlideTo(slide.centre, ZoomToFill(slide, aspect), kGlideSeconds);
    }
    return true;
  }
  return false;
}

void SlidePanel::Draw(const Viewport& viewport, FontStore& fonts, float aspect) const {
  const float zoom = viewport.Zoom();
  const Vector2 pointer = GetMousePosition();

  for (const Slide& slide : slides_) {
    const Rectangle bounds = Bounds(slide, aspect);
    const Vector2 topLeft = viewport.ToScreen({bounds.x, bounds.y});
    const Rectangle rect = {topLeft.x, topLeft.y, bounds.width * zoom, bounds.height * zoom};

    // Off screen entirely: nothing to draw, and a deck can be long.
    if (rect.x > GetScreenWidth() || rect.y > GetScreenHeight() ||
        rect.x + rect.width < 0.0f || rect.y + rect.height < 0.0f) {
      continue;
    }

    // The rim is permanent here rather than appearing on hover: it is what says
    // this part of the canvas is a slide. Nothing else is drawn: whatever is on
    // a slide is on the canvas, and the slide is only the frame round it.
    DrawRim(rect, zoom, PointOnRim(rect, pointer));
  }

  // The button that throws a slide away, in the corner of the slide whose rim
  // is being pointed at. It says what it does: this one is not a gesture to
  // learn, and there is nothing to undo it with.
  if (hoverSlide_ >= 0 && hoverSlide_ < static_cast<int>(slides_.size())) {
    const Rectangle rim =
        OnScreen(slides_[static_cast<std::size_t>(hoverSlide_)], viewport, aspect);
    const Rectangle close = DeleteButtonRect(rim, fonts);
    if (close.width > 0.0f &&
        (PointOnRim(rim, pointer) || CheckCollisionPointRec(pointer, close))) {
      const bool hot = CheckCollisionPointRec(pointer, close);
      DrawRectangleRounded(close, 0.3f, 8, hot ? kDropHot : kDropBg);
      const float width = text::Measure(fonts, Face::MainRegular, kDeleteLabel, kDeleteEm);
      text::Draw(fonts, Face::MainRegular, kDeleteLabel,
                 {close.x + (close.width - width) * 0.5f,
                  close.y + close.height * 0.5f + kDeleteEm * 0.34f},
                 kDeleteEm, kDropMark);
    }
  }

  // The new-slide button last, so it is never behind a slide it stands next to.
  const Rectangle button = ButtonRect(viewport);
  if (button.width < 2.0f) return;
  const bool hovered = CheckCollisionPointRec(pointer, button);
  DrawRectangleRounded(button, 0.3f, 10, hovered ? kAccentHover : kAccent);

  const float em = kButtonEm * zoom;
  if (em < 3.0f) return;
  const float width = text::Measure(fonts, Face::MainRegular, "New slide", em);
  text::Draw(fonts, Face::MainRegular, "New slide",
             {button.x + (button.width - width) * 0.5f,
              button.y + button.height * 0.5f + em * 0.34f},
             em, RAYWHITE);
}

}  // namespace sb
