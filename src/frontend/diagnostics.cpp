#include "frontend/diagnostics.hpp"
#include <iomanip>
#include <iostream>

namespace symir {

  void printMessage(
      std::ostream &os, const std::string &src, const SourceSpan &span, const std::string &msg,
      DiagLevel level
  ) {
    std::string_view sv = src;

    // Sanity check bounds
    if (span.begin.offset > sv.size()) {
      os << "Error: " << msg << " (invalid source location)\n";
      return;
    }

    // Find line start
    size_t lineStart = span.begin.offset;
    while (lineStart > 0 && sv[lineStart - 1] != '\n') {
      lineStart--;
    }

    // Find line end
    size_t lineEnd = span.begin.offset;
    while (lineEnd < sv.size() && sv[lineEnd] != '\n') {
      lineEnd++;
    }

    std::string_view lineContent = sv.substr(lineStart, lineEnd - lineStart);

    std::string levelStr =
        (level == DiagLevel::Error) ? "error" : (level == DiagLevel::Warning ? "warning" : "note");

    os << span.begin.line << ":" << span.begin.col << ": " << levelStr << ": " << msg << "\n";
    os << lineContent << "\n";

    // Print caret
    // We strictly follow the byte offset for column alignment relative to the printed line.
    // We replicate the whitespace prefix exactly to handle tabs correctly if they exist.
    for (size_t i = 0; i < (span.begin.offset - lineStart); ++i) {
      if (lineContent[i] == '\t')
        os << '\t';
      else
        os << ' ';
    }
    os << "^\n";
  }

} // namespace symir
