#include "compiler.h"

#include <cctype>
#include <cstddef>
#include <string>

namespace sb::compiler {

Diagnostic ParseGlslLog(const std::string& log, const ShaderLines& where) {
  Diagnostic out;

  std::size_t start = 0;
  while (start <= log.size()) {
    std::size_t end = log.find('\n', start);
    if (end == std::string::npos) end = log.size();
    std::string text = log.substr(start, end - start);
    start = end + 1;

    // Warnings come through the same channel; only the first error is shown.
    std::string lowered = text;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lowered.find("error") == std::string::npos) {
      if (end >= log.size()) break;
      continue;
    }

    // The log arrives wrapped in the host's own wording. Everything before the
    // driver's message is noise, and it contains digits that would otherwise be
    // mistaken for the location.
    static const std::string kHandoff = "Compile error: ";
    const std::size_t handoff = text.find(kHandoff);
    if (handoff != std::string::npos) text = text.substr(handoff + kHandoff.size());

    // The location is the first `<source>(<line>)` or `<source>:<line>` in the
    // line, where the source index is always 0 for us: one string was compiled.
    // Parsed into locals rather than straight into the result, because the
    // second error must not overwrite the first.
    int         atLine = 0;
    int         atColumn = 1;
    bool        haveLine = false;
    std::size_t i = 0;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) == 0)) ++i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) ++i;
    if (i < text.size() && (text[i] == '(' || text[i] == ':')) {
      ++i;
      int line = 0;
      while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
        line = line * 10 + (text[i] - '0');
        haveLine = true;
        ++i;
      }
      if (haveLine) {
        atLine = line;
        // Mesa writes the column as `0:12(5):`; nobody else gives one.
        if (i < text.size() && text[i] == '(') {
          ++i;
          int  column = 0;
          bool haveColumn = false;
          while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
            column = column * 10 + (text[i] - '0');
            haveColumn = true;
            ++i;
          }
          if (haveColumn) atColumn = std::max(1, column);
        }
      }
    }

    // The message is whatever follows the last colon of the location prefix.
    const std::size_t colon = text.find(':', i);
    std::string message = (colon == std::string::npos) ? text : text.substr(colon + 1);
    const std::size_t firstAt = message.find_first_not_of(" \t");
    const std::size_t lastAt = message.find_last_not_of(" \t\r");
    message = (firstAt == std::string::npos) ? text : message.substr(firstAt, lastAt - firstAt + 1);

    // Some drivers then say "error C1503:" before getting to the point. The
    // balloon is already the place errors appear; it does not need saying.
    if (message.size() > 5) {
      std::string head = message.substr(0, 5);
      for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const std::size_t inner = message.find(':');
      if (head == "error" && inner != std::string::npos) {
        const std::size_t after = message.find_first_not_of(" \t", inner + 1);
        if (after != std::string::npos) message = message.substr(after);
      }
    }

    // A box's lines appear twice in the shader -- hoisted above main, and again
    // inside it -- so which range the driver's line falls in decides which one
    // it was. The body first: an error there is about what a line *does*.
    int boxLine = 1;
    if (haveLine) {
      if (where.count > 0 && atLine >= where.bodyAt && atLine < where.bodyAt + where.count) {
        boxLine = std::max(1, atLine - where.bodyAt);
      } else if (where.count > 0 && atLine >= where.functionsAt &&
                 atLine < where.functionsAt + where.count) {
        boxLine = std::max(1, atLine - where.functionsAt);
      } else {
        boxLine = std::max(1, atLine - where.bodyAt);
      }
    }

    if (!out.present) {
      out.present = true;
      out.line = boxLine;
      out.column = atColumn;
      out.message = message;
    } else if (out.secondary.empty()) {
      // Said with its line, because it is not where the caret is pointing.
      out.secondary = "line " + std::to_string(boxLine) + ": " + message;
      break;
    }
    if (end >= log.size()) break;
  }
  return out;
}

}  // namespace sb::compiler
