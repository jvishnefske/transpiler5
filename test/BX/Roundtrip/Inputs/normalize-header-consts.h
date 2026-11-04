// FR-264 header constants for normalize-header-consts.cpp (brunsli
// constants.h / context.h shapes): a chain through initializers, one used
// only by a header function, an unreferenced scalar and an array constant
// that the lazy import must leave alone.
#pragma once
#include <cstddef>
#include <cstdint>

namespace brunsli {
static const std::size_t kMaxHuffmanBits = 15u;
static const std::size_t kBrunsliSignatureSize = 6;
static const int kPrecBits = 13;
static const int kPrec = 1 << kPrecBits;
static const int kUnused = 99;
static const std::uint8_t kTable[] = {1, 2, 3};
static const int kScaleBias = 2;
inline int Scale(int x) {
    return x * kScaleBias;
}
}  // namespace brunsli
