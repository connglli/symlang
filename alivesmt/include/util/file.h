#pragma once
#include <string>

namespace alivesmt::util {
  std::string
  get_random_filename(const std::string &dir, const std::string &ext, const char *prefix);
}
