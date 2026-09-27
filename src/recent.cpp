#include "recent.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "settings.h"

namespace sb::recent {
namespace {

// Beside the user's other settings rather than beside the canvas: which files
// you opened lately is a fact about you, not about any one canvas, and a
// canvas has to stay movable.
std::filesystem::path StorePath() {
  const std::filesystem::path folder = settings::Folder();
  return folder.empty() ? std::filesystem::path("recent.txt") : folder / "recent.txt";
}

// Two spellings of one file are one entry. Windows does not care about case or
// about which way the slashes lean, so neither does the list.
std::string Key(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
#ifdef _WIN32
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
  }
  return path;
}

std::vector<std::string> Read() {
  std::vector<std::string> out;
  std::ifstream file(StorePath(), std::ios::binary);
  if (!file) return out;
  std::string line;
  while (std::getline(file, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) out.push_back(line);
  }
  return out;
}

void Write(const std::vector<std::string>& paths) {
  std::ofstream file(StorePath(), std::ios::binary | std::ios::trunc);
  if (!file) return;
  for (const std::string& path : paths) file << path << "\n";
}

}  // namespace

std::vector<std::string> List() {
  std::vector<std::string> out;
  std::error_code ec;
  for (const std::string& path : Read()) {
    if (!std::filesystem::exists(path, ec)) continue;
    out.push_back(path);
    if (static_cast<int>(out.size()) >= kMaxRecent) break;
  }
  return out;
}

void Remember(const std::string& path) {
  if (path.empty()) return;

  std::vector<std::string> paths = Read();
  const std::string key = Key(path);
  paths.erase(std::remove_if(paths.begin(), paths.end(),
                             [&key](const std::string& p) { return Key(p) == key; }),
              paths.end());
  paths.insert(paths.begin(), path);
  if (static_cast<int>(paths.size()) > kMaxRecent) {
    paths.resize(static_cast<std::size_t>(kMaxRecent));
  }
  Write(paths);
}

}  // namespace sb::recent
