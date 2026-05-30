#pragma once

// [v0.2.2] FuncPool — loads rysmith-emitted (.sir + .json) pairs from a
// directory so rylink can pick functions to assemble into whole programs.
//
// One PoolEntry represents one parsed rysmith descriptor together with the
// path of its companion `.sir` file. Programs are NOT parsed eagerly; the
// loader only validates the descriptor and confirms the .sir exists. The
// caller parses .sir lazily on demand (via the regular frontend), so a
// large pool stays cheap to enumerate.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "reify/func_desc.hpp"

namespace symir::reify {

  struct PoolEntry {
    // Parsed descriptor (carries name, signature, realizations).
    FuncDescriptor desc;
    // Filesystem path of the companion .sir file. The descriptor lists
    // each realization by basename only; this is the directory we resolve
    // those basenames against.
    std::filesystem::path descDir;
    // Realization index → absolute path of the .sir file for that init.
    // Length matches desc.realizations.size(). Realizations whose .sir
    // is missing on disk are skipped (the entry is kept iff at least
    // one realization remains).
    std::vector<std::filesystem::path> sirPaths;
  };

  struct FuncPool {
    std::vector<PoolEntry> entries;
  };

  // Scan `dir` for *.json descriptors and the matching .sir files. Returns
  // an empty pool (and prints a warning to stderr) if the directory is
  // missing or contains no usable entries.
  //
  // Why no exception on empty: a degenerate pool is a normal startup-time
  // diagnostic in rylink ("you pointed me at the wrong directory"), not a
  // crash condition. The CLI driver decides what to do with an empty pool.
  FuncPool loadFuncPool(const std::filesystem::path &dir);

} // namespace symir::reify
