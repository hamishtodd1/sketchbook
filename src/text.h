// Plain-text drawing on top of FontStore.
//
// raylib's DrawText* work from a single Font at a fixed base size; these go
// through the store instead so text picks up an atlas rasterised near its final
// on-screen size and stays sharp under zoom.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "fontstore.h"
#include "raylib.h"

namespace sb::text {

// Decodes one UTF-8 scalar starting at `i`, advancing it past the sequence.
int NextCodepoint(std::string_view s, std::size_t* i);

float Measure(FontStore& fonts, Face face, std::string_view utf8, float emPx);

// `pen` is the left edge of the baseline.
void Draw(FontStore& fonts, Face face, std::string_view utf8, Vector2 pen, float emPx,
          Color tint);

float Ascent(FontStore& fonts, Face face, float emPx);
float Descent(FontStore& fonts, Face face, float emPx);
float LineHeight(FontStore& fonts, Face face, float emPx);

// Greedy word wrap. A word longer than `maxWidth` on its own is broken rather
// than allowed to overflow.
std::vector<std::string> Wrap(FontStore& fonts, Face face, std::string_view utf8, float emPx,
                              float maxWidth);

}  // namespace sb::text
