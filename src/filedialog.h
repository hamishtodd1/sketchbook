// Native open/save dialogs.
//
// Like the other Win32 corners of this program, this never includes raylib.h:
// <windows.h> also declares Rectangle, CloseWindow and ShowCursor.
#pragma once

#include <string>

namespace sb::filedialog {

// Both return an empty string if the user cancelled, or if the platform has no
// dialog to offer.
std::string Save(const std::string& suggestedName);
std::string Open();

// The same dialog, filtered for the things that can become a window on the
// canvas. Dropping a file on the window does this too, but a tablet has
// nothing to drop with.
std::string OpenMedia();

// Filtered for sound -- music, a voiceover -- to lay under a video (editor.h).
std::string OpenSound();

}  // namespace sb::filedialog
