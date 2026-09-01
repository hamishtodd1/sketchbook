// Windows multi-touch backend.
//
// Deliberately free of raylib types: <windows.h> and raylib.h both declare
// Rectangle, CloseWindow and ShowCursor, so the two headers cannot meet in one
// translation unit. Keeping this interface primitive lets touch.cpp talk to
// Win32 without ever including both.
#pragma once

namespace sb::touch::win32 {

struct RawPoint {
  unsigned int id;
  float        x;  // client-area pixels
  float        y;
};

// `hwnd` is the raw HWND, passed as void* to keep windows.h out of the header.
bool Install(void* hwnd);
void Shutdown();

// Copies the live contacts out; returns how many were written.
int Snapshot(RawPoint* out, int maxPoints);

}  // namespace sb::touch::win32
