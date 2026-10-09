// What the app remembers about you between runs, apart from the recent list.
//
// A fact about the person rather than about a canvas, so it lives with the
// user's settings (`%APPDATA%/sketchbook`) and never in a `.sketch`: a canvas
// has to stay movable, and one that carried how its author likes the window
// would carry it to everyone it was sent to. Raylib-free, like the rest of the
// layer that talks to the outside world.
#pragma once

#include <filesystem>
#include <string>

namespace sb::settings {

// Where the user's settings live, made if it is not there yet. Empty if there
// is nowhere to put it, and then a setting is simply not kept.
std::filesystem::path Folder();

// Whether the app was last left filling the screen. Written the moment it
// changes rather than at exit, so a crash does not forget it.
bool Fullscreen();
void RememberFullscreen(bool on);

// The Claude API key, sealed to this Windows account (secret.h). Empty if none
// has been given, or if it was sealed somewhere else. Remembering an empty one
// forgets it.
std::string ApiKey();
void        RememberApiKey(const std::string& key);

}  // namespace sb::settings
