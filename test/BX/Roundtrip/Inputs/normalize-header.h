// Project header for ../normalize-header.cpp (FR-254 R4). Not a test: lit
// excludes every Inputs/ directory.
#pragma once
#include <cstddef>
#include <cstdint>

inline std::size_t Put16(std::uint8_t* dst, std::size_t off, int x) {
    dst[off] = static_cast<std::uint8_t>(x >> 8);
    dst[off + 1] = static_cast<std::uint8_t>(x);
    return off + 2;
}

static int Square(int a) {
    return a * a;
}

inline int SumSquares(int a, int b) {
    return Square(a) + Square(b);
}

inline int NeverCalled(int a) {
    return a + 1;
}

namespace codec {
inline int Shift(int v) {
    return v << 2;
}
}  // namespace codec
