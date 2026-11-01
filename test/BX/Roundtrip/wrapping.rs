// FR-250: canonical Rust -> C++ under --check-roundtrip. Pins the unsigned
// rows read from the Rust side: .wrapping_add/sub/mul/neg <=> plain C++
// unsigned operators (which wrap by definition), `x = x.wrapping_op(e);`
// <=> `x op= e;`, `i += 1` <=> `++i;`, `as` <=> static_cast, and
// `let` / `let mut` <=> `const T` / `T`.
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
fn fnv1a(mut h: u32, b: u8) -> u32 {
    h ^= b as u32;
    h = h.wrapping_mul(16777619u32);
    return h;
}

fn mix64(x: u64) -> u64 {
    let mut z: u64 = x.wrapping_add(11400714819323198485u64);
    z = (z ^ (z >> 30)).wrapping_mul(13787848793156543929u64);
    return z ^ (z >> 31);
}

fn main() {
    let mut h: u32 = 2166136261u32;
    for i in 0..5 {
        h = fnv1a(h, (i + 97i32) as u8);
    }
    let mut n: usize = 0usize;
    n = n.wrapping_sub(1usize);
    let neg: u32 = 5u32.wrapping_neg();
    let mut k: i64 = 0i64;
    k += 1i64;
    print!("{} {} {} {} {}\n", h, mix64(42u64), n, neg, k);
}

// CHECK: #include <cstdio>
// CHECK-EMPTY:
// CHECK-NEXT: unsigned fnv1a(unsigned h, const unsigned char b) {
// CHECK-NEXT:     h ^= static_cast<unsigned>(b);
// CHECK-NEXT:     h *= 16777619U;
// CHECK-NEXT:     return h;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: unsigned long long mix64(const unsigned long long x) {
// CHECK-NEXT:     unsigned long long z = x + 11400714819323198485ULL;
// CHECK-NEXT:     z = (z ^ (z >> 30)) * 13787848793156543929ULL;
// CHECK-NEXT:     return z ^ (z >> 31);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: int main() {
// CHECK-NEXT:     unsigned h = 2166136261U;
// CHECK-NEXT:     for (int i = 0; i < 5; ++i) {
// CHECK-NEXT:         h = fnv1a(h, static_cast<unsigned char>(i + 97));
// CHECK-NEXT:     }
// CHECK-NEXT:     std::size_t n = static_cast<std::size_t>(0);
// CHECK-NEXT:     --n;
// CHECK-NEXT:     const unsigned neg = -5U;
// CHECK-NEXT:     long long k = 0LL;
// CHECK-NEXT:     ++k;
// CHECK-NEXT:     std::printf("%u %llu %zu %u %lld\n", h, mix64(42ULL), n, neg, k);
// CHECK-NEXT:     return 0;
// CHECK-NEXT: }
