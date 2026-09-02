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
// The **new slide** button sits on the canvas rather than in a toolbar, and
// clicking it leaves a slide where the button was and drops the button below
// it. Clicking again does the same, so a deck grows downwards and is somewhere
// you can scroll through rather than a list beside the work.
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
// canvas is told which region went, and how far the deck below it came up, so
// it can do the same to the windows. Same division of labour as NewSlide: the
// panel knows about slides, the canvas knows about what is on them.
struct SlideRemoval {
  bool      asked = false;
  Rectangle bounds{};      // world, the slide that went
  float     shift = 0.0f;  // how far everything below it moved up
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
  Rectangle DeleteButtonRect() const;
  static Rectangle OnScreen(const Slide& slide, const Viewport& viewport, float aspect);
  void      RemoveSlide(int index, float aspect);

  std::vector<Slide> slides_;

  // The button, and the glide it is making down past the slide just made.
  Vector2 buttonPos_ = DefaultButtonPosition();
  Vector2 buttonFrom_{};
  Vector2 buttonTo_{};
  float   buttonElapsed_ = 0.0f;
  bool    buttonMoving_ = false;

  NewSlide     pending_;
  SlideRemoval removal_;

  // The slide whose rim is being pointed at, and where its delete button sits.
  // The button follows the pointer along the rim and then stays put once the
  // pointer leaves the rim for it -- a button that ran away from the click
  // meant to press it would be no button at all.
  int     hoverSlide_ = -1;
  Vector2 deleteAt_{};
};

}  // namespace sb
