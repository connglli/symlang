#pragma once

#include <stdexcept>
#include <string>

namespace symir {

  /**
   * Exit codes returned by all SymIR tools (symiri, symirc, symirsolve).
   * Specific exit codes let the test framework (and callers) distinguish
   * *why* a run failed without parsing stderr text.
   */
  namespace ExitCode {
    constexpr int Success = 0;
    constexpr int Error = 1;             // generic / internal error
    constexpr int LexError = 2;          // lexer rejected the source
    constexpr int ParseError = 3;        // parser rejected the source
    constexpr int StaticError = 4;       // type / semantic / CFG analysis error
    constexpr int UndefinedBehavior = 5; // runtime undefined behavior
    constexpr int RequireViolation = 6;  // require assertion failed
  } // namespace ExitCode

  /**
   * Thrown by the interpreter when a path triggers Undefined Behavior.
   * The message begins with a short UB category tag (e.g. "Null pointer
   * dereference") so it can be displayed to the user as-is.
   */
  struct UndefinedBehaviorError : std::runtime_error {
    explicit UndefinedBehaviorError(const std::string &msg) : std::runtime_error(msg) {}
  };

  /**
   * Thrown by the interpreter when a `require` assertion fails at runtime.
   */
  struct RequireViolationError : std::runtime_error {
    explicit RequireViolationError(const std::string &msg) : std::runtime_error(msg) {}
  };

} // namespace symir
