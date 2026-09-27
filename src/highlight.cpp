#include "highlight.h"

#include <cctype>
#include <string>
#include <unordered_set>

#include "theme.h"

namespace sb::highlight {
namespace {

bool IsIdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}
bool IsIdentPart(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

const std::unordered_set<std::string>& Keywords() {
  static const std::unordered_set<std::string> set = {
      "break",  "buffer", "case",  "const",   "continue", "default", "discard", "do",
      "else",   "flat",   "for",   "highp",   "if",       "in",      "inout",   "layout",
      "lowp",   "mediump", "out",  "precision", "return", "shared",  "smooth",  "struct",
      "switch", "uniform", "while"};
  return set;
}

const std::unordered_set<std::string>& Types() {
  static const std::unordered_set<std::string> set = {
      "bool",  "bvec2", "bvec3", "bvec4", "double", "float", "int",   "ivec2", "ivec3",
      "ivec4", "mat2",  "mat3",  "mat4",  "uint",   "uvec2", "uvec3", "uvec4", "vec2",
      "vec3",  "vec4",  "void",  "sampler2D"};
  return set;
}

const std::unordered_set<std::string>& Constants() {
  static const std::unordered_set<std::string> set = {"true", "false"};
  return set;
}

}  // namespace

std::vector<Span> Glsl(std::string_view line) {
  std::vector<Span> spans;
  std::size_t i = 0;

  auto push = [&spans](std::size_t begin, std::size_t end, Color color) {
    if (end > begin) spans.push_back({begin, end, color});
  };

  while (i < line.size()) {
    const std::size_t start = i;
    const char c = line[i];

    // A line comment swallows the rest of the line.
    if (c == '/' && i + 1 < line.size() && (line[i + 1] == '/' || line[i + 1] == '*')) {
      push(start, line.size(), kTokComment);
      break;
    }

    if (c == '"' || c == '\'') {
      const char quote = c;
      ++i;
      while (i < line.size()) {
        if (line[i] == '\\' && i + 1 < line.size()) { i += 2; continue; }
        if (line[i] == quote) { ++i; break; }
        ++i;
      }
      push(start, i, kTokString);
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
        (c == '.' && i + 1 < line.size() &&
         std::isdigit(static_cast<unsigned char>(line[i + 1])) != 0)) {
      while (i < line.size() &&
             (std::isalnum(static_cast<unsigned char>(line[i])) != 0 || line[i] == '.')) {
        ++i;
      }
      push(start, i, kTokNumber);
      continue;
    }

    if (IsIdentStart(c)) {
      while (i < line.size() && IsIdentPart(line[i])) ++i;
      const std::string word(line.substr(start, i - start));

      Color color = kCodeFg;
      if (Types().count(word) != 0) {
        color = kTokType;
      } else if (Keywords().count(word) != 0) {
        color = kTokKeyword;
      } else if (Constants().count(word) != 0) {
        color = kTokNumber;
      } else {
        // An identifier followed by "(" is being called or declared.
        std::size_t peek = i;
        while (peek < line.size() && line[peek] == ' ') ++peek;
        if (peek < line.size() && line[peek] == '(') color = kTokFunction;
      }
      push(start, i, color);
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) != 0) ++i;
      push(start, i, kCodeFg);
      continue;
    }

    // Punctuation. Brackets and separators stay in the foreground colour;
    // genuine operators take the accent, which is what Monokai does.
    ++i;
    const bool structural = (c == '(' || c == ')' || c == '{' || c == '}' || c == '[' ||
                             c == ']' || c == ';' || c == ',' || c == '.');
    push(start, i, structural ? kCodeFg : kTokOperator);
  }

  return spans;
}

}  // namespace sb::highlight
