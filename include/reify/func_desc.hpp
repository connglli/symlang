#pragma once

// [v0.2.2] Function descriptor — sidecar metadata that rysmith writes
// next to each generated `.sir` file (`func_<id>_<i>.json`) so the
// future `rylink` driver can wire callers/callees without re-parsing
// the SIR source. Both the writer (used by rysmith) and the reader
// (used by rylink) live here so the on-disk schema has a single
// canonical owner.

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>
#include "ast/ast.hpp"

namespace symir::reify {

  struct FuncDescriptor {
    // 6-char hex generation ID this descriptor was emitted under.
    std::string id;
    // Canonical sigil-prefixed function name, e.g. `@func_a3f9c2_0`.
    std::string name;
    // Return type, serialized in SIR surface syntax (e.g. `i32`,
    // `ptr i32`, `@struct_a3f9c2_0`, `<4> i32`).
    std::string retType;

    struct Param {
      std::string name; // e.g. `%pa0`
      std::string type; // SIR surface syntax
    };

    std::vector<Param> params;

    // [v0.2.2] Top-level `syms` was dropped: rysmith re-generates the
    // statement list (and therefore the sym set) per init, so any
    // single value here would reflect only the last init and disagree
    // with the other realizations' actual sym sets. Per-realization
    // `symValues` carries the canonical, init-specific data; anyone
    // who needs the kind/type metadata should reach for the .sir
    // file's `sym` declarations directly.

    // Block-label path the solver concretized along (`["^entry", "^b1",
    // "^exit"]`). Consistent across all realizations of one descriptor
    // because rysmith samples the path once per attempt.
    std::vector<std::string> path;

    struct Struct {
      std::string name; // `@struct_<id>_<j>`

      struct Field {
        std::string name; // `f0`
        std::string type; // SIR surface syntax
      };

      std::vector<Field> fields;
    };

    std::vector<Struct> structs;

    // [v0.2.2] One per-init record. Carries the concrete .sir file
    // the solver produced for this realization plus the solver-
    // synthesised values for each parameter and the return
    // expression. Values are stringified in the same format
    // rysmith writes into the SOLVED header (decimal ints / hex
    // floats / std::to_string(double) full-precision), so a
    // consumer can hand them straight to `symiri ... -- v0 v1`.
    struct Realization {
      std::string file; // basename only, relative to descriptor dir
      // Parameter values keyed by the parameter's local-id (e.g.
      // `%pa0`). Empty when the function takes no parameters.
      std::vector<std::pair<std::string, std::string>> paramValues;
      // Sym values keyed by the sym's local-id (e.g. `%?s0`). One
      // entry per sym declared in *this* realization's program. The
      // sym set may differ across realizations because rysmith
      // re-generates statements per init; consult the companion
      // .sir file to recover sym kinds/types if you need them.
      std::vector<std::pair<std::string, std::string>> symValues;
      // Solved value of the `ret` expression on the chosen path.
      // Empty string when the path's terminator is not `ret <expr>;`.
      std::string retValue;
    };

    std::vector<Realization> realizations;
  };

  // Serialize a descriptor as JSON to `outPath`. Overwrites any
  // existing file. Throws on I/O error.
  void writeFuncDescriptor(const std::filesystem::path &outPath, const FuncDescriptor &d);

  // Convenience overload that builds the FuncDescriptor from an
  // in-memory Program + bookkeeping the caller already has. The
  // function named `@<funcName>` must exist in `prog.funs`; if it
  // doesn't, nothing is written and `false` is returned.
  bool writeFuncDescriptorFromProgram(
      const std::filesystem::path &outPath, const std::string &funcName, const symir::Program &prog,
      const std::vector<std::string> &pathLabels,
      const std::vector<FuncDescriptor::Realization> &realizations, const std::string &genId
  );

  // Parse a descriptor JSON. Returns nullopt on parse error (callers
  // typically log and skip). The JSON shape is intentionally narrow
  // (flat string-valued fields plus a few small arrays) so a tiny
  // hand-written parser is sufficient — no dependency added.
  std::optional<FuncDescriptor> readFuncDescriptor(const std::filesystem::path &path);
  std::optional<FuncDescriptor> parseFuncDescriptor(const std::string &json);

} // namespace symir::reify
