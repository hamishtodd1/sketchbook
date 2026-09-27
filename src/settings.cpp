#include "settings.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace sb::settings {
namespace {

// One `key value` per line. A key this build does not know is kept as it was,
// so an older build writing its own settings leaves a newer one's alone.
using Store = std::map<std::string, std::string>;

std::filesystem::path StorePath() {
  const std::filesystem::path folder = Folder();
  return folder.empty() ? std::filesystem::path() : folder / "settings.txt";
}

Store Read() {
  Store store;
  const std::filesystem::path path = StorePath();
  if (path.empty()) return store;
  std::ifstream file(path, std::ios::binary);
  std::string   line;
  while (std::getline(file, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const std::size_t space = line.find(' ');
    if (space == std::string::npos || space == 0) continue;
    store[line.substr(0, space)] = line.substr(space + 1);
  }
  return store;
}

void Write(const Store& store) {
  const std::filesystem::path path = StorePath();
  if (path.empty()) return;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  for (const auto& [key, value] : store) file << key << " " << value << "\n";
}

}  // namespace

std::filesystem::path Folder() {
#ifdef _WIN32
  const char* base = std::getenv("APPDATA");
#else
  const char* base = std::getenv("HOME");
#endif
  if (base == nullptr || *base == '\0') return {};
  const std::filesystem::path folder = std::filesystem::path(base) / "sketchbook";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  if (ec) return {};
  return folder;
}

bool Fullscreen() {
  const Store store = Read();
  const auto found = store.find("fullscreen");
  return found != store.end() && found->second == "1";
}

void RememberFullscreen(bool on) {
  Store store = Read();
  store["fullscreen"] = on ? "1" : "0";
  Write(store);
}

}  // namespace sb::settings
