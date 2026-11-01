// FR-259: cheap syntactic breadth, C++ -> canonical Rust under
// --check-roundtrip (laws 1-2 on BOTH sides), plus the canonical C++. Pins:
// the <cstdint>/<cstddef> names in EVERY position -- a reference's pointee
// (`const std::vector<std::uint8_t>&`), a return type (`std::uint32_t`),
// template arguments, locals -- are their builtin type (normal form N20; the
// canonical C++ spells `unsigned char` / `unsigned` / `unsigned short` /
// `short` / `long long` / `std::size_t`); `static` / `inline` free functions
// are plain `fn`s at the root and `static` inside a namespace is the
// anonymous-namespace member `namespace {` <=> private `fn` (N21); `auto` /
// `const auto` / `const auto&` range-for take clang's deduced type (N22);
// `c ? a : b` <=> `if c { a } else { b }` with the usual arithmetic
// conversions explicit and a nested `?:` an `else if` (parenthesized in C++);
// `x++` / `++x` / `n--` / `--pos` used as values (an index, a `while`
// condition, a store value, a `?:` condition) <=> `{ x += 1; x - 1 }`-style
// blocks, wrapping for unsigned; i16 / u16 (`short` / `unsigned short`).
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
// CHECK:      fn checksum(data: &Vec<u8>) -> u32 {
// CHECK-NEXT:     let mut s: u32 = 0u32;
// CHECK-NEXT:     for b in data.iter() {
// CHECK-NEXT:         s = s.wrapping_mul(31u32).wrapping_add((*b) as u32);
// CHECK-NEXT:     }
// CHECK-NEXT:     return s;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn narrow(mut x: i64) -> i16 {
// CHECK-NEXT:     return x as i16;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: mod codec {
// CHECK-NEXT:     fn skip(v: &Vec<u16>, mut pos: usize) -> usize {
// CHECK-NEXT:         while (pos < v.len()) && ((v[pos] as i32) == 0) {
// CHECK-NEXT:             pos = pos.wrapping_add(1usize);
// CHECK-NEXT:         }
// CHECK-NEXT:         return pos;
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn first(v: &Vec<u16>) -> usize {
// CHECK-NEXT:         return skip(v, 0usize);
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn pick(mut c: bool, mut i: i32, mut u: u32, mut ch: i8) -> i32 {
// CHECK-NEXT:     let mut r: u32 = if c { i as u32 } else { u };
// CHECK-NEXT:     let q: i8 = if (ch as i32) > 97 { ch } else { 97i8 };
// CHECK-NEXT:     return (r as i32) + (if (q as i32) == 122 { 1 } else if i < 0 { -1 } else { 0 });
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn drain(mut n: i32, out: &mut Vec<i32>, total: &mut u32) -> i32 {
// CHECK-NEXT:     let mut pos: usize = 0usize;
// CHECK-NEXT:     let mut i: i32 = 0;
// CHECK-NEXT:     let mut seen: u32 = 2u32;
// CHECK-NEXT:     while ({ n -= 1; n + 1 }) > 0 {
// CHECK-NEXT:         out[{ pos = pos.wrapping_add(1usize); pos.wrapping_sub(1usize) }] = { i += 1; i };
// CHECK-NEXT:         *total = (*total).wrapping_add(if ({ seen = seen.wrapping_add(1u32); seen.wrapping_sub(1u32) }) > 3u32 { 2u32 } else { 1u32 });
// CHECK-NEXT:     }
// CHECK-NEXT:     return i + (({ pos = pos.wrapping_sub(1usize); pos }) as i32);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut d: Vec<u8> = vec![7u8, 200u8];
// CHECK-NEXT:     let mut z: Vec<u16> = vec![0u16, 0u16, 9u16];
// CHECK-NEXT:     let mut out: Vec<i32> = vec![0, 0, 0];
// CHECK-NEXT:     let mut t: u32 = 1u32;
// CHECK-NEXT:     let mut k: i32 = drain(3, &mut out, &mut t);
// CHECK-NEXT:     print!("{} {} {} {} {} {}\n", checksum(&d), narrow(70000i64) as i32, crate::codec::first(&z) as i32, pick(true, -2, 5u32, 113i8), k, t);
// CHECK-NEXT: }
// CPP:      #include <cstdio>
// CPP-NEXT: #include <vector>
// CPP-EMPTY:
// CPP-NEXT: unsigned checksum(const std::vector<unsigned char>& data) {
// CPP-NEXT:     unsigned s = 0U;
// CPP-NEXT:     for (const unsigned char& b : data) {
// CPP-NEXT:         s = (s * 31U) + static_cast<unsigned>(b);
// CPP-NEXT:     }
// CPP-NEXT:     return s;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: short narrow(long long x) {
// CPP-NEXT:     return static_cast<short>(x);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: namespace codec {
// CPP-EMPTY:
// CPP-NEXT: namespace {
// CPP-EMPTY:
// CPP-NEXT: std::size_t skip(const std::vector<unsigned short>& v, std::size_t pos) {
// CPP-NEXT:     while ((pos < v.size()) && (static_cast<int>(v[pos]) == 0)) {
// CPP-NEXT:         ++pos;
// CPP-NEXT:     }
// CPP-NEXT:     return pos;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace
// CPP-EMPTY:
// CPP-NEXT: std::size_t first(const std::vector<unsigned short>& v) {
// CPP-NEXT:     return skip(v, static_cast<std::size_t>(0));
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace codec
// CPP-EMPTY:
// CPP-NEXT: int pick(bool c, int i, unsigned u, char ch) {
// CPP-NEXT:     unsigned r = c ? static_cast<unsigned>(i) : u;
// CPP-NEXT:     const char q = static_cast<int>(ch) > 97 ? ch : 'a';
// CPP-NEXT:     return static_cast<int>(r) + (static_cast<int>(q) == 122 ? 1 : (i < 0 ? -1 : 0));
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int drain(int n, std::vector<int>& out, unsigned& total) {
// CPP-NEXT:     std::size_t pos = static_cast<std::size_t>(0);
// CPP-NEXT:     int i = 0;
// CPP-NEXT:     unsigned seen = 2U;
// CPP-NEXT:     while (n-- > 0) {
// CPP-NEXT:         out[pos++] = ++i;
// CPP-NEXT:         total += seen++ > 3U ? 2U : 1U;
// CPP-NEXT:     }
// CPP-NEXT:     return i + static_cast<int>(--pos);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     std::vector<unsigned char> d = {static_cast<unsigned char>(7), static_cast<unsigned char>(200)};
// CPP-NEXT:     std::vector<unsigned short> z = {static_cast<unsigned short>(0), static_cast<unsigned short>(0), static_cast<unsigned short>(9)};
// CPP-NEXT:     std::vector<int> out = {0, 0, 0};
// CPP-NEXT:     unsigned t = 1U;
// CPP-NEXT:     int k = drain(3, out, t);
// CPP-NEXT:     std::printf("%u %d %d %d %d %u\n", checksum(d), static_cast<int>(narrow(70000LL)), static_cast<int>(::codec::first(z)), pick(true, -2, 5U, 'q'), k, t);
// CPP-NEXT:     return 0;
// CPP-NEXT: }
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static std::uint32_t checksum(const std::vector<std::uint8_t>& data) {
    std::uint32_t s = 0;
    for (const auto& b : data) {
        s = s * 31 + b;
    }
    return s;
}

inline std::int16_t narrow(std::int64_t x) {
    return static_cast<std::int16_t>(x);
}

namespace codec {
static std::size_t skip(const std::vector<std::uint16_t>& v, std::size_t pos) {
    while (pos < v.size() && v[pos] == 0) {
        pos++;
    }
    return pos;
}

std::size_t first(const std::vector<std::uint16_t>& v) {
    return skip(v, 0);
}
}  // namespace codec

int pick(bool c, int i, unsigned u, char ch) {
    auto r = c ? i : u;
    const auto q = ch > 'a' ? ch : 'a';
    return static_cast<int>(r) + (q == 'z' ? 1 : i < 0 ? -1 : 0);
}

int drain(int n, std::vector<int>& out, std::uint32_t& total) {
    std::size_t pos = 0;
    int i = 0;
    std::uint32_t seen = 2;
    while (n-- > 0) {
        out[pos++] = ++i;
        total += seen++ > 3U ? 2U : 1U;
    }
    return i + static_cast<int>(--pos);
}

int main() {
    std::vector<std::uint8_t> d = {7, 200};
    std::vector<std::uint16_t> z = {0, 0, 9};
    std::vector<int> out = {0, 0, 0};
    std::uint32_t t = 1;
    int k = drain(3, out, t);
    std::printf("%u %d %d %d %d %u\n", checksum(d), narrow(70000), static_cast<int>(codec::first(z)), pick(true, -2, 5U, 'q'), k, t);
    return 0;
}
