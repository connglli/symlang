#pragma once
#include <iostream>
#include <string>

namespace alivesmt::util::config {
  inline std::ostream &dbg() { return std::cerr; }

  extern bool skip_smt;
  extern std::string smt_benchmark_dir;
} // namespace alivesmt::util::config
