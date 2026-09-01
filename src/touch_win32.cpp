#if defined(_WIN32)

#include "touch_win32.h"

// WM_POINTER* and the GetPointer* entry points are Windows 8 and later.
#ifndef WINVER
#define WINVER 0x0602
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

namespace sb::touch::win32 {
namespace {

// Not every MinGW/SDK combination exposes these, so pin them locally.
#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#endif
#ifndef WM_POINTERDOWN
#define WM_POINTERDOWN 0x0246
#endif
#ifndef WM_POINTERUP
#define WM_POINTERUP 0x0247
#endif
#ifndef WM_POINTERLEAVE
#define WM_POINTERLEAVE 0x024A
#endif
#ifndef WM_POINTERCAPTURECHANGED
#define WM_POINTERCAPTURECHANGED 0x024C
#endif

constexpr int kMaxContacts = 10;

using GetPointerTypeFn = BOOL(WINAPI*)(UINT32, POINTER_INPUT_TYPE*);
using GetPointerInfoFn = BOOL(WINAPI*)(UINT32, POINTER_INFO*);

struct State {
  HWND             hwnd = nullptr;
  WNDPROC          previous = nullptr;
  GetPointerTypeFn getPointerType = nullptr;
  GetPointerInfoFn getPointerInfo = nullptr;
  RawPoint         points[kMaxContacts]{};
  int              count = 0;
  bool             installed = false;
};

State& Get() {
  static State state;
  return state;
}

void Remove(unsigned int id) {
  State& s = Get();
  for (int i = 0; i < s.count; ++i) {
    if (s.points[i].id != id) continue;
    for (int j = i; j + 1 < s.count; ++j) s.points[j] = s.points[j + 1];
    --s.count;
    return;
  }
}

void Upsert(unsigned int id, float x, float y) {
  State& s = Get();
  for (int i = 0; i < s.count; ++i) {
    if (s.points[i].id == id) {
      s.points[i].x = x;
      s.points[i].y = y;
      return;
    }
  }
  if (s.count < kMaxContacts) {
    s.points[s.count] = RawPoint{id, x, y};
    ++s.count;
  }
}

void HandlePointer(UINT message, WPARAM wParam) {
  State& s = Get();
  if (s.getPointerType == nullptr || s.getPointerInfo == nullptr) return;

  const UINT32 id = static_cast<UINT32>(LOWORD(wParam));
  POINTER_INPUT_TYPE type = PT_POINTER;
  if (!s.getPointerType(id, &type)) return;
  if (type != PT_TOUCH && type != PT_PEN) return;

  if (message == WM_POINTERUP || message == WM_POINTERLEAVE ||
      message == WM_POINTERCAPTURECHANGED) {
    Remove(id);
    return;
  }

  POINTER_INFO info{};
  if (!s.getPointerInfo(id, &info)) return;

  POINT pt = info.ptPixelLocation;  // screen pixels
  ScreenToClient(s.hwnd, &pt);
  Upsert(id, static_cast<float>(pt.x), static_cast<float>(pt.y));
}

LRESULT CALLBACK Hook(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  State& s = Get();
  switch (message) {
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
    case WM_POINTERLEAVE:
    case WM_POINTERCAPTURECHANGED:
      HandlePointer(message, wParam);
      break;
    case WM_KILLFOCUS:
      s.count = 0;
      break;
    default:
      break;
  }
  // Always chain on: GLFW still needs every message, and the legacy mouse
  // events synthesised from touch are left intact so a tap can drive the menu.
  return CallWindowProcW(s.previous, hwnd, message, wParam, lParam);
}

}  // namespace

bool Install(void* hwnd) {
  State& s = Get();
  if (s.installed) return true;
  if (hwnd == nullptr) return false;
  s.hwnd = static_cast<HWND>(hwnd);

  // Resolved at runtime: linking these directly would stop the app starting on
  // a machine whose user32 predates them.
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) return false;
  s.getPointerType = reinterpret_cast<GetPointerTypeFn>(
      reinterpret_cast<void*>(GetProcAddress(user32, "GetPointerType")));
  s.getPointerInfo = reinterpret_cast<GetPointerInfoFn>(
      reinterpret_cast<void*>(GetProcAddress(user32, "GetPointerInfo")));
  if (s.getPointerType == nullptr || s.getPointerInfo == nullptr) return false;

  s.previous = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(s.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Hook)));
  s.installed = (s.previous != nullptr);
  return s.installed;
}

void Shutdown() {
  State& s = Get();
  if (!s.installed) return;
  SetWindowLongPtrW(s.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s.previous));
  s.installed = false;
  s.count = 0;
}

int Snapshot(RawPoint* out, int maxPoints) {
  State& s = Get();
  const int n = (s.count < maxPoints) ? s.count : maxPoints;
  for (int i = 0; i < n; ++i) out[i] = s.points[i];
  return n;
}

}  // namespace sb::touch::win32

#endif  // _WIN32
