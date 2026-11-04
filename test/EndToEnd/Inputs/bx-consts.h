// FR-264 project-header constants for bx-consts.cpp: a chain through
// initializers (kPrec -> kPrecBits), a constant reached only through a
// header function (kRound via Quantize), and ones the lazy import must never
// bring in (kUnused, the array kTable).
#pragma once
#include <cstddef>
#include <cstdint>

namespace codec {
static const int kPrecBits = 13;
static const int kPrec = 1 << kPrecBits;
static const std::size_t kMaxBits = 15u;
static const int kRound = 3;
static const int kUnused = 99;
static const std::uint8_t kTable[] = {1, 2, 3};
inline int Quantize(int v) {
    return (v + kRound) / (kRound + 1);
}
}  // namespace codec
