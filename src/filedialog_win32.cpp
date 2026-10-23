#include "filedialog.h"

#if defined(_WIN32)

#ifndef WINVER
#define WINVER 0x0602
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <vector>

namespace sb::filedialog {
namespace {

// "Sketchbook canvas (*.sketch)\0*.sketch\0All files\0*.*\0\0" -- the double
// NUL terminator is why this cannot be a plain string literal.
const wchar_t kFilter[] =
    L"Sketchbook canvas (*.sketch)\0*.sketch\0All files (*.*)\0*.*\0\0";

const wchar_t kMediaFilter[] =
    L"Pictures and video\0"
    L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif;*.qoi;*.psd;*.hdr;*.pic;"
    L"*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.wmv;*.mpg;*.mpeg\0"
    L"All files (*.*)\0*.*\0\0";

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

std::string Narrow(const wchar_t* wide) {
  if (wide == nullptr || wide[0] == L'\0') return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return {};
  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
  return out;
}

OPENFILENAMEW Prepare(std::vector<wchar_t>* buffer) {
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFilter = kFilter;
  ofn.nFilterIndex = 1;
  ofn.lpstrFile = buffer->data();
  ofn.nMaxFile = static_cast<DWORD>(buffer->size());
  ofn.lpstrDefExt = L"sketch";
  return ofn;
}

}  // namespace

std::string Save(const std::string& suggestedName) {
  std::vector<wchar_t> buffer(1024, L'\0');
  const std::wstring initial = Widen(suggestedName);
  if (!initial.empty() && initial.size() < buffer.size() - 1) {
    std::copy(initial.begin(), initial.end(), buffer.begin());
  }

  OPENFILENAMEW ofn = Prepare(&buffer);
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (GetSaveFileNameW(&ofn) == 0) return {};
  return Narrow(buffer.data());
}

std::string Open() {
  std::vector<wchar_t> buffer(1024, L'\0');
  OPENFILENAMEW ofn = Prepare(&buffer);
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (GetOpenFileNameW(&ofn) == 0) return {};
  return Narrow(buffer.data());
}

std::string OpenMedia() {
  std::vector<wchar_t> buffer(1024, L'\0');
  OPENFILENAMEW ofn = Prepare(&buffer);
  ofn.lpstrFilter = kMediaFilter;
  ofn.lpstrDefExt = nullptr;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (GetOpenFileNameW(&ofn) == 0) return {};
  return Narrow(buffer.data());
}

std::string OpenSound() {
  static const wchar_t kSoundFilter[] =
      L"Sound\0*.mp3;*.wav;*.ogg;*.flac;*.m4a;*.aac;*.opus;*.wma;*.mp4;*.mkv;*.mov\0"
      L"All files (*.*)\0*.*\0\0";
  std::vector<wchar_t> buffer(1024, L'\0');
  OPENFILENAMEW ofn = Prepare(&buffer);
  ofn.lpstrFilter = kSoundFilter;
  ofn.lpstrDefExt = nullptr;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (GetOpenFileNameW(&ofn) == 0) return {};
  return Narrow(buffer.data());
}

}  // namespace sb::filedialog

#else  // ----------------------------------------------------------- fallback

namespace sb::filedialog {
std::string Save(const std::string&) { return {}; }
std::string Open() { return {}; }
std::string OpenMedia() { return {}; }
std::string OpenSound() { return {}; }
}  // namespace sb::filedialog

#endif
