// Slides.
//
// A slide is a rectangle of the canvas with a permanent rim round it -- a
// region declared to be worth looking at on its own. Clicking that rim glides
// the camera until the slide fills the window, so presenting is navigating: the
// same infinite canvas, framed.
//
// Its size comes from the camera's own default. The height is what one screen
// showed at zoom 1, and the width is that times the aspect the deck is being
// made for, so a slide viewed at its own scale is exactly a screenful.
//
// The **new slide** button sits on the canvas rather than in a toolbar, at the
// **end of the deck**: right beside the last slide rather than out in the
// middle of where the next one will go, so it reads as the place the deck
// stops. Clicking it puts a slide clear of that one, with a gap between them,
// and walks the button along to the new end. A deck therefore grows **left to
// right**, and is a row you can scroll along rather than a list beside the
// work.
//
// Which slide follows which is decided by where they sit, left to right --
// nothing else records an order, and nothing else needs to.
#pragma once

#include <string>
#include <vector>

#include "fontstore.h"
#include "raylib.h"
#include "viewport.h"

namespace sb {

// One screenful at zoom 1, with the window this app opens at. A slide is that
// tall; how wide depends on the aspect being worked in.
inline constexpr float kSlideHeight = 800.0f;
// Between one slide and the next, and between the last slide and the button.
inline constexpr float kSlideGap = 60.0f;

struct Slide {
  Vector2 centre{};
};

// What the panel is telling the app it just did. A slide's title and its body
// are ordinary text boxes -- movable, editable, resizable like everything else
// -- and windows belong to the canvas, not to the panel. So the panel says a
// slide has appeared and where, and the app puts the words on it.
struct NewSlide {
  bool    made = false;
  Vector2 centre{};
};

// A slide the user asked to throw away. By the time this is reported the panel
// has already taken the slide out of the deck and closed the gap behind it; the
// canvas is told which region went, and how far the deck after it came back, so
// it can do the same to the windows. Same division of labour as NewSlide: the
// panel knows about slides, the canvas knows about what is on them.
struct SlideRemoval {
  bool      asked = false;
  Rectangle bounds{};      // world, the slide that went
  float     shift = 0.0f;  // how far everything to the right of it moved left
};

class SlidePanel {
 public:
  // Where the button starts: just left of where the camera opens, so it is
  // there to be found without being in the way of the middle of the canvas.
  static Vector2 DefaultButtonPosition() { return {-520.0f, 0.0f}; }

  void Reset();

  // Taken once: asking clears it, so a slide is furnished exactly once.
  NewSlide     TakeNewSlide();
  SlideRemoval TakeRemoval();

  // Where the words start on a slide, in world units. Only the title needs one:
  // the body goes under whatever height the title turned out to be.
  static Vector2 TitleAnchor(Vector2 centre, float aspect);
  // The size slide text is set at, and how wide it may run before wrapping.
  static float   TextEm();
  static float   ContentWidth(float aspect);

  // Returns true if it took the pointer.
  bool HandleInput(const Viewport& viewport, FontStore& fonts, float aspect,
                   Viewport* camera);

  // The slide the camera is framing, or -1: the one a click on its rim would
  // have put it at.
  int SlideAtCamera(const Viewport& camera, float aspect) const;
  // Flicks to the slide before or after that one, instantly -- flicking through
  // a deck is not travelling to it, and a glide per keypress would be seasick.
  // False if the camera is not on a slide, or there is no next one.
  bool Step(int direction, Viewport* camera, float aspect) const;
  // Starts a deck where the button would have been. The button only stands at
  // the *end* of a deck, so before there is one it has nowhere to stand and the
  // first slide is asked for from the menu instead.
  void AddSlide(float aspect, Viewport* camera);
  void Advance(float seconds);
  void Draw(const Viewport& viewport, FontStore& fonts, float aspect) const;

  // The world rectangle a slide occupies at this aspect.
  static Rectangle Bounds(const Slide& slide, float aspect);
  // What the camera must be at for that slide to fill the window, with its rim
  // just outside it.
  static float ZoomToFill(const Slide& slide, float aspect);

  // --- persistence --------------------------------------------------------
  const std::vector<Slide>& Slides() const { return slides_; }
  Vector2                   ButtonPosition() const { return buttonPos_; }
  void Adopt(std::vector<Slide> slides, Vector2 buttonPosition);

 private:
  Rectangle ButtonRect(const Viewport& viewport) const;
  // Where a slide's delete button sits, given where the slide is on screen.
  // Returns a zero-size rectangle when the slide is too small on screen to
  // carry one.
  static Rectangle DeleteButtonRect(Rectangle rim, FontStore& fonts);
  static Rectangle OnScreen(const Slide& slide, const Viewport& viewport, float aspect);
  void      RemoveSlide(int index, float aspect);
  // The slides in the order they read, which is the order they sit in.
  std::vector<int> Order() const;
  // Where the next slide goes, and where the button belongs once it is there.
  Vector2   NextCentre(float aspect) const;
  Vector2   ButtonHome(float aspect) const;

  std::vector<Slide> slides_;

  // The button, and the glide it is making along past the slide just made.
  Vector2 buttonPos_ = DefaultButtonPosition();
  Vector2 buttonFrom_{};
  Vector2 buttonTo_{};
  float   buttonElapsed_ = 0.0f;
  bool    buttonMoving_ = false;

  NewSlide     pending_;
  SlideRemoval removal_;

  // The slide whose rim is being pointed at, which is the one showing its
  // delete button. The button lives in that slide's bottom-right corner, so it
  // is somewhere you can go and find rather than something to chase.
  int hoverSlide_ = -1;
};

}  // namespace sb
