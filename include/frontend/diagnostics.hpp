#pragma once

#include <string>
#include <vector>
#include "ast/ast.hpp"

namespace symir {

  enum class DiagLevel { Error, Warning, Note };

  struct Diagnostic {
    DiagLevel level;
    std::string message;
    SourceSpan span;
  };

  struct DiagBag {
    std::vector<Diagnostic> diags;

    void error(const std::string &msg, SourceSpan sp) {
      diags.push_back(Diagnostic{DiagLevel::Error, msg, sp});
    }

    void warn(const std::string &msg, SourceSpan sp) {
      diags.push_back(Diagnostic{DiagLevel::Warning, msg, sp});
    }

    void note(const std::string &msg, SourceSpan sp) {
      diags.push_back(Diagnostic{DiagLevel::Note, msg, sp});
    }

    bool hasErrors() const {
      for (const auto &d: diags)
        if (d.level == DiagLevel::Error)
          return true;
      return false;
    }
  };

} // namespace symir
