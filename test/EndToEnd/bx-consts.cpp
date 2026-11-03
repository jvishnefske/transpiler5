// REQUIRES: cargo
// FR-264: emitrust-bx --normalize --project-root, C++ -> Rust, byte-diffed
// against clang++ of the ORIGINAL program (this file plus its project header
// Inputs/bx-consts.h) -- turning namespace-scope constants into Rust `const`
// items must not change a single printed byte. Rows: unsigned constants
// whose initializers wrap (`0xFFFFFFFFu + 2u`, `1u - 3u`, `-5u`, a
// `uint32_t` shift that drops high bits) and are then mixed into running
// unsigned arithmetic; narrowing initializers (`const uint8_t = 300`,
// `const int8_t = 200`) and an int8 constant in arithmetic; a `double`
// constant printed with %.3f and a `static_cast<int>(kSqrt2 * kPrec)`
// (brunsli's kSqrt2FixedPoint shape) whose kPrec comes from the header chain
// kPrecBits -> kPrec; a `size_t` shift constant (NextTableBitSize shape); a
// constant as a counting-loop bound and as a `while` bound from the header;
// a constant used inside a lambda (never captured); `static const`,
// `constexpr` and `inline constexpr` spellings; a namespace constant used
// from outside its namespace. The subset has no `main(argc, ...)`, so the
// values come from an LCG; UB-free (signed arithmetic stays in range, every
// overflow is on unsigned values).
// RUN: %emitrust-bx --normalize --project-root=%S --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_consts > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: not grep -q unsafe %t.crate/src/main.rs
// RUN: not grep -E -q "kUnused|kTable" %t.crate/src/main.rs
#include <cstdint>
#include <cstdio>

#include "Inputs/bx-consts.h"

const std::uint32_t kWrapAdd = 0xFFFFFFFFu + 2u;
const std::uint32_t kWrapSub = 1u - 3u;
const std::uint32_t kWrapNeg = -5u;
const std::uint32_t kShl = 0xF0000001u << 4;
const std::uint8_t kNarrow = 300;
const std::int8_t kSigned = 200;
static const double kSqrt2 = 1.414213562;
static const int kFixed = static_cast<int>(kSqrt2 * codec::kPrec);
const std::size_t kLeft = std::size_t(1) << (codec::kMaxBits - std::size_t(9));
constexpr int kRows = 12;
inline constexpr int kMod = 30011;
const bool kWide = kFixed > kRows;

namespace mix {
const std::uint32_t kMul = 2654435761u;
std::uint32_t Hash(std::uint32_t h, std::uint32_t v) {
    return (h ^ v) * kMul + kWrapAdd;
}
}  // namespace mix

int next(int x) {
    return (x * 1103 + 12345) % kMod;
}

int main() {
    int seed = 7;
    std::uint32_t h = kWrapSub;
    auto scale = [](int v) -> int { return v * kSigned + kNarrow; };
    for (int i = 0; i < kRows; ++i) {
        seed = next(seed);
        h = mix::Hash(h, static_cast<std::uint32_t>(seed)) - kWrapNeg;
        std::printf("%d %u %d %d\n", seed, h, scale(seed % 100), codec::Quantize(seed));
    }
    std::size_t bits = 0;
    while (bits < codec::kMaxBits) {
        bits = bits + 4;
    }
    std::printf("%u %u %u %u\n", kWrapAdd, kWrapSub, kWrapNeg, kShl);
    std::printf("%d %d %d %.3f\n", static_cast<int>(kNarrow), static_cast<int>(kSigned), kFixed, kSqrt2);
    std::printf("%zu %zu %d %u\n", kLeft, bits, static_cast<int>(kWide), mix::kMul);
    return 0;
}
