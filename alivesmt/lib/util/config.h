#pragma once
#include <iostream>
#include <string>

namespace util::config {
  inline std::ostream &dbg() { return std::cerr; }

  extern bool skip_smt;
  extern std::string smt_benchmark_dir;
} // namespace util::config
