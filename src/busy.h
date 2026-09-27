// The busy cursor, while a shader compiles.
//
// A compile happens on the thread that draws -- it has to, since compiling
// means talking to the graphics context -- so for as long as it takes nothing
// on the canvas can say so: no frame is drawn until it is over. The pointer is
// the one thing the system goes on drawing regardless, so it is what says the
// app is working, in the shape everyone already reads as "wait".
//
// A compile of a few milliseconds is over before the shape could be seen, so in
// practice it only appears for the slow ones -- which are the ones worth
// knowing about.
//
// Win32, and raylib-free: <windows.h> and raylib.h cannot meet in one file.
#pragma once

namespace sb::busy {

// Shows the busy cursor until the matching End. Nesting is allowed; only the
// outermost pair changes anything.
void Begin();
void End();

// For a scope: busy from construction to destruction.
struct Scope {
  Scope() { Begin(); }
  ~Scope() { End(); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

}  // namespace sb::busy
