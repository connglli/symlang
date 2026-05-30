#pragma once

// [v0.2.2] Shared 6-hex-char generation-ID helper used by rysmith and
// rylink. Both tools tag every artifact (function names, struct names,
// per-program output directories) with this ID so that outputs from
// independent runs can coexist in the same filesystem layout without
// renaming. Centralising the generator keeps the format identical
// across tools: any future consumer of the ID can re-use the same
// function rather than rolling its own and drifting on width / casing.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace symir::reify {

  // Returns a freshly-generated 6-character lowercase-hex string drawn
  // from `rng`. Format matches `%06x` of a 24-bit unsigned value.
  inline std::string genHexId(std::mt19937 &rng) {
    std::uniform_int_distribution<uint32_t> d(0, 0xFFFFFFu);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06x", d(rng));
    return std::string(buf);
  }

} // namespace symir::reify
