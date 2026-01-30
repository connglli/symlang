#include "util/config.h"
#include "util/file.h"

namespace util::config {
  bool skip_smt = false;
  std::string smt_benchmark_dir = "";
} // namespace util::config

namespace util {
  std::string
  get_random_filename(const std::string &dir, const std::string &ext, const char *prefix) {
    return dir + "/" + prefix + "." + ext;
  }
} // namespace util
