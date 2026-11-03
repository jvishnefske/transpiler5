// FR-264: namespace-scope scalar constants, C++ -> canonical C++ and Rust
// under --check-roundtrip (laws 1-2 on both sides through real clang).
// Pinned: `static const`, `constexpr`, `inline constexpr` and plain `const`
// are ONE normal form, printed `const T k = e;` <=> `const k: T = e;` (at
// namespace scope a const object already has internal linkage, and the
// subset has no context `constexpr` adds); the constants block sits after
// the structs and before every function, in C++ definition order; a constant
// in a named namespace is `pub const` in its `mod`, one in an anonymous
// namespace a private `const`; a reference is bare in its own namespace and
// absolute otherwise (`::kA` <=> `crate::kA`); unsigned arithmetic keeps the
// wrapping_* methods (const fn) and a narrowing initializer keeps its
// conversion (`300i32 as u8`); a const counting-loop bound; a lambda using a
// constant captures NOTHING (`[]` -- C++ cannot capture a non-automatic);
// the crate opts out of the upper-case lint once, first. The printed C++ is
// a fixed point and the printed Rust prints the same C++.
// RUN: %emitrust-bx --check-roundtrip --emit=rust %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: FileCheck --check-prefix=CPP --input-file=%t.cpp %s
// RUN: not grep -E "static const|constexpr" %t.cpp
// RUN: %emitrust-bx --emit=cpp %t.cpp | diff %t.cpp -
// RUN: %emitrust-bx --emit=rust %s -o %t.rs
// RUN: %emitrust-bx --emit=cpp %t.rs | diff %t.cpp -
#include <cstdint>
#include <cstdio>

static const int kA = 7;
constexpr int kB = kA * 3;
inline constexpr int kC = 2;
const std::uint32_t kWrap = 0xFFFFFFFFu + 2u;
const std::uint32_t kNeg = -5u;
const std::uint8_t kNarrow = 300;
static const double kScale = 1.414213562;
static const int kFixed = static_cast<int>(kScale * (1 << 13));
const std::size_t kLen = std::size_t(1) << (std::size_t(15) - std::size_t(9));
const bool kBig = kB > 20;

namespace codec {
const int kShift = kC + 1;
namespace {
const int kMask = (1 << kShift) - 1;
}
int Pack(int v) {
    return (v & kMask) | ::kA;
}
}  // namespace codec

int main() {
    int s = 0;
    for (int i = 0; i < kB; ++i) {
        s += codec::Pack(i);
    }
    auto add = [](int x) -> int { return x + kA; };
    std::printf("%d %d %u %u %d\n", s, add(kC), kWrap, kNeg, (int)kNarrow);
    std::printf("%d %zu %d %.3f %d\n", kFixed, kLen, (int)kBig, kScale, codec::kShift);
    return 0;
}

// CHECK:      #![allow(non_upper_case_globals)]
// CHECK-EMPTY:
// CHECK-NEXT: const kA: i32 = 7;
// CHECK-EMPTY:
// CHECK-NEXT: const kB: i32 = kA * 3;
// CHECK-EMPTY:
// CHECK-NEXT: const kC: i32 = 2;
// CHECK-EMPTY:
// CHECK-NEXT: const kWrap: u32 = 4294967295u32.wrapping_add(2u32);
// CHECK-EMPTY:
// CHECK-NEXT: const kNeg: u32 = 5u32.wrapping_neg();
// CHECK-EMPTY:
// CHECK-NEXT: const kNarrow: u8 = 300i32 as u8;
// CHECK-EMPTY:
// CHECK-NEXT: const kScale: f64 = 1.414213562;
// CHECK-EMPTY:
// CHECK-NEXT: const kFixed: i32 = (kScale * ((1i32 << 13i32) as f64)) as i32;
// CHECK-EMPTY:
// CHECK-NEXT: const kLen: usize = 1usize << 15usize.wrapping_sub(9usize);
// CHECK-EMPTY:
// CHECK-NEXT: const kBig: bool = kB > 20;
// CHECK-EMPTY:
// CHECK-NEXT: mod codec {
// CHECK-NEXT:     pub const kShift: i32 = crate::kC + 1;
// CHECK-EMPTY:
// CHECK-NEXT:     const kMask: i32 = (1 << kShift) - 1;
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn Pack(mut v: i32) -> i32 {
// CHECK-NEXT:         return (v & kMask) | crate::kA;
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK:          for i in 0..kB {
// CHECK:          let add = |mut x: i32| -> i32 {
// CHECK-NEXT:         return x + kA;
// CHECK:          print!("{} {} {} {} {}\n", kFixed, kLen, kBig as i32, bx::fmt_f(kScale, 3), crate::codec::kShift);
// CHECK-NOT:  allow(non_upper_case_globals)

// CPP:      #include <cstdio>
// CPP-EMPTY:
// CPP-NEXT: const int kA = 7;
// CPP-NEXT: const int kB = kA * 3;
// CPP-NEXT: const int kC = 2;
// CPP-NEXT: const unsigned kWrap = 4294967295U + 2U;
// CPP-NEXT: const unsigned kNeg = -5U;
// CPP-NEXT: const unsigned char kNarrow = static_cast<unsigned char>(300);
// CPP-NEXT: const double kScale = 1.414213562;
// CPP-NEXT: const int kFixed = static_cast<int>(kScale * static_cast<double>(1 << 13));
// CPP-NEXT: const std::size_t kLen = static_cast<std::size_t>(1) << (static_cast<std::size_t>(15) - static_cast<std::size_t>(9));
// CPP-NEXT: const bool kBig = kB > 20;
// CPP-NEXT: namespace codec {
// CPP-NEXT: const int kShift = ::kC + 1;
// CPP-NEXT: namespace {
// CPP-NEXT: const int kMask = (1 << kShift) - 1;
// CPP-NEXT: }  // namespace
// CPP-NEXT: }  // namespace codec
// CPP:      int Pack(int v) {
// CPP-NEXT:     return (v & kMask) | ::kA;
// CPP:          auto add = [](int x) -> int {
