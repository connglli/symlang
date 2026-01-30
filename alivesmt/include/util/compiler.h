#pragma once
#include <cstdint>
#include <cstdlib>

#define UNREACHABLE() __builtin_unreachable()
#define ENSURE(x)                                                                                  \
  if (!(x))                                                                                        \
  abort()

namespace alivesmt::util {
  inline bool is_power2(uint64_t n) { return n && !(n & (n - 1)); }

  inline bool is_power2(uint64_t n, unsigned *log) {
    if (!is_power2(n))
      return false;
    *log = 0;
    while (n >>= 1)
      (*log)++;
    return true;
  }

  inline bool is_power2(uint64_t n, uint64_t *log) {
    if (!is_power2(n))
      return false;
    *log = 0;
    while (n >>= 1)
      (*log)++;
    return true;
  }

  // ilog2_ceil for exprs.h
  inline unsigned ilog2_ceil(uint64_t n, bool) {
    if (n == 0)
      return 0;
    unsigned log = 0;
    if (is_power2(n, &log))
      return log;
    return log + 1; // Simplistic approximation or implementation
  }
} // namespace alivesmt::util
