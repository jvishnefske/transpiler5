// Project header for ../bx-normalize-header.cpp (FR-254 R4). Not a test: lit
// excludes every Inputs/ directory. woff2/brunsli-shaped store/load helpers.
#pragma once
#include <cstddef>
#include <cstdint>

inline std::size_t Store16(std::uint8_t* dst, std::size_t off, int x) {
    dst[off] = static_cast<std::uint8_t>(x >> 8);
    dst[off + 1] = static_cast<std::uint8_t>(x);
    return off + 2;
}

inline std::size_t StoreU32(std::uint8_t* dst, std::size_t off, std::uint32_t x) {
    dst[off] = static_cast<std::uint8_t>(x >> 24);
    dst[off + 1] = static_cast<std::uint8_t>(x >> 16);
    dst[off + 2] = static_cast<std::uint8_t>(x >> 8);
    dst[off + 3] = static_cast<std::uint8_t>(x);
    return off + 4;
}

static std::uint32_t Load8(const std::uint8_t* src, std::size_t off) {
    return static_cast<std::uint32_t>(src[off]);
}

inline std::uint32_t LoadU32(const std::uint8_t* src, std::size_t off) {
    return (Load8(src, off) << 24) | (Load8(src, off + 1) << 16) | (Load8(src, off + 2) << 8) |
           Load8(src, off + 3);
}

// a non-canonical loop: R1 (for->while) applies inside the imported body
inline int Log2Floor(std::uint32_t n) {
    int r = -1;
    for (; n != 0; n >>= 1) {
        r = r + 1;
    }
    return r;
}

namespace codec {
inline int Mix(int a, int b) {
    return (a * 31 + b) % 10007;
}
}  // namespace codec

inline int NeverCalled(int a) {
    return a + 1;
}
