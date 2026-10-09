#include "secret.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

namespace sb::secret {
namespace {

constexpr char kHex[] = "0123456789abcdef";

std::string ToHex(const BYTE* data, DWORD size) {
  std::string out;
  out.reserve(static_cast<std::size_t>(size) * 2);
  for (DWORD i = 0; i < size; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

int Nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string Seal(const std::string& plain) {
  if (plain.empty()) return {};
  DATA_BLOB in{static_cast<DWORD>(plain.size()),
               reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
  DATA_BLOB out{};
  if (!CryptProtectData(&in, L"sketchbook", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                        &out)) {
    return {};
  }
  std::string hex = ToHex(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return hex;
}

std::string Open(const std::string& sealed) {
  if (sealed.empty() || sealed.size() % 2 != 0) return {};
  std::string bytes;
  bytes.reserve(sealed.size() / 2);
  for (std::size_t i = 0; i < sealed.size(); i += 2) {
    const int high = Nibble(sealed[i]);
    const int low = Nibble(sealed[i + 1]);
    if (high < 0 || low < 0) return {};
    bytes.push_back(static_cast<char>((high << 4) | low));
  }
  DATA_BLOB in{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &out)) {
    return {};
  }
  std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return plain;
}

}  // namespace sb::secret

#else

// Nowhere else is built yet; with no sealing to be had, nothing is kept.
namespace sb::secret {
std::string Seal(const std::string&) { return {}; }
std::string Open(const std::string&) { return {}; }
}  // namespace sb::secret

#endif
