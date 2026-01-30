#include "util/config.h"
#include "util/file.h"

namespace alivesmt::util::config {
  bool skip_smt = false;
  std::string smt_benchmark_dir = "";
} // namespace alivesmt::util::config

namespace alivesmt::util {
  std::string
  get_random_filename(const std::string &dir, const std::string &ext, const char *prefix) {
    return dir + "/" + prefix + "." + ext;
  }
} // namespace alivesmt::util
