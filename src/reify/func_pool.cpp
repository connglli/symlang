#include "reify/func_pool.hpp"

#include <iostream>

namespace fs = std::filesystem;

namespace symir::reify {

  FuncPool loadFuncPool(const fs::path &dir) {
    FuncPool pool;
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
      std::cerr << "rylink: pool dir does not exist: " << dir << "\n";
      return pool;
    }

    for (const auto &de: fs::directory_iterator(dir)) {
      if (!de.is_regular_file())
        continue;
      const auto &p = de.path();
      if (p.extension() != ".json")
        continue;
      auto maybeDesc = readFuncDescriptor(p);
      if (!maybeDesc) {
        std::cerr << "rylink: descriptor parse failed, skipping: " << p << "\n";
        continue;
      }
      PoolEntry e;
      e.desc = std::move(*maybeDesc);
      e.descDir = dir;

      // Resolve each realization's .sir basename → absolute path. Drop
      // realizations whose file is missing on disk; if all are missing
      // the entry is skipped entirely.
      std::vector<FuncDescriptor::Realization> keptRzs;
      keptRzs.reserve(e.desc.realizations.size());
      for (const auto &rz: e.desc.realizations) {
        fs::path sirPath = dir / rz.file;
        if (!fs::exists(sirPath))
          continue;
        e.sirPaths.push_back(sirPath);
        keptRzs.push_back(rz);
      }
      if (keptRzs.empty()) {
        std::cerr << "rylink: no .sir files found for descriptor " << p << ", skipping\n";
        continue;
      }
      e.desc.realizations = std::move(keptRzs);

      pool.entries.push_back(std::move(e));
    }

    return pool;
  }

} // namespace symir::reify
