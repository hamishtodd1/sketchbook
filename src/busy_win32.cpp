#include "busy.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sb::busy {
namespace {

int     g_depth = 0;
HCURSOR g_before = nullptr;

}  // namespace

void Begin() {
  if (g_depth++ > 0) return;
  // SetCursor takes effect at once, without the message loop -- which is
  // exactly what is needed, since the message loop is what the compile is
  // holding up.
  g_before = SetCursor(LoadCursor(nullptr, IDC_WAIT));
}

void End() {
  if (g_depth == 0 || --g_depth > 0) return;
  // Back to whatever it was, which may be the hidden cursor of a pan or the
  // closed hand we draw ourselves -- not simply the arrow.
  SetCursor(g_before);
  g_before = nullptr;
}

}  // namespace sb::busy
