#include "touch.h"

#include <algorithm>

#if defined(_WIN32)
#include "touch_win32.h"
#endif

namespace sb::touch {
namespace {

Point g_points[kMaxPoints]{};
int   g_count = 0;
bool  g_available = false;

}  // namespace

#if defined(_WIN32)

void Install() { g_available = win32::Install(GetWindowHandle()); }
void Shutdown() {
  win32::Shutdown();
  g_available = false;
  g_count = 0;
}
bool Available() { return g_available; }

int Count() {
  if (!g_available) { g_count = 0; return 0; }
  win32::RawPoint raw[kMaxPoints];
  g_count = win32::Snapshot(raw, kMaxPoints);
  for (int i = 0; i < g_count; ++i) {
    g_points[i].id = raw[i].id;
    g_points[i].position = {raw[i].x, raw[i].y};
  }
  return g_count;
}

#else

// Elsewhere, fall back to whatever raylib reports. That is real multi-touch on
// Android and web, and a mouse-shaped single point on other desktops.
void Install() { g_available = true; }
void Shutdown() {
  g_available = false;
  g_count = 0;
}
bool Available() { return g_available; }

int Count() {
  g_count = std::min(GetTouchPointCount(), kMaxPoints);
  for (int i = 0; i < g_count; ++i) {
    g_points[i].id = static_cast<unsigned int>(GetTouchPointId(i));
    g_points[i].position = GetTouchPosition(i);
  }
  return g_count;
}

#endif

const Point* Points() { return g_points; }

}  // namespace sb::touch
