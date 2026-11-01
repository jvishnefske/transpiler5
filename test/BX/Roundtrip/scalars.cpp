// FR-250: the scalar rule rows, C++ -> canonical Rust, under --check-roundtrip
// (laws 1-2 asserted on BOTH sides for this input). Pins: every type row
// (char is SIGNED -> i8, never u8), every C++ implicit conversion surfacing as
// an explicit `as` cast, bool-context ints normalizing to `!= 0`, the literal
// suffix rule, and unsigned arithmetic spelled .wrapping_*().
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
#include <cstdio>
#include <cstddef>

long long widen(int a, char c, unsigned char uc, bool b) {
    long long r = a + c;
    r = r * uc;
    if (b) {
        r = -r;
    }
    return r;
}

unsigned mix(unsigned u, unsigned long long w, std::size_t z, double d) {
    u = u * 2654435761u + 7u;
    w = w - u;
    z = z % 7;
    int t = d;
    u ^= ~u >> 3;
    return u + static_cast<unsigned>(t) + static_cast<unsigned>(w) + static_cast<unsigned>(z);
}

int main() {
    const char c = 'A';
    int n = c;
    while (n) {
        n = n / 2;
    }
    std::printf("%lld %u\n", widen(-3, c, 200, true), mix(5u, 9ull, 44, 3.75));
    return 0;
}

// CHECK: fn widen(mut a: i32, mut c: i8, mut uc: u8, mut b: bool) -> i64 {
// CHECK-NEXT:     let mut r: i64 = (a + (c as i32)) as i64;
// CHECK-NEXT:     r *= uc as i64;
// CHECK-NEXT:     if b {
// CHECK-NEXT:         r = -r;
// CHECK-NEXT:     }
// CHECK-NEXT:     return r;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn mix(mut u: u32, mut w: u64, mut z: usize, mut d: f64) -> u32 {
// CHECK-NEXT:     u = u.wrapping_mul(2654435761u32).wrapping_add(7u32);
// CHECK-NEXT:     w = w.wrapping_sub(u as u64);
// CHECK-NEXT:     z %= 7usize;
// CHECK-NEXT:     let mut t: i32 = d as i32;
// CHECK-NEXT:     u ^= !u >> 3;
// CHECK-NEXT:     return u.wrapping_add(t as u32).wrapping_add(w as u32).wrapping_add(z as u32);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let c: i8 = 65i8;
// CHECK-NEXT:     let mut n: i32 = c as i32;
// CHECK-NEXT:     while n != 0 {
// CHECK-NEXT:         n /= 2;
// CHECK-NEXT:     }
// CHECK-NEXT:     print!("{} {}\n", widen(-3, c, 200u8, true), mix(5u32, 9u64, 44usize, 3.75));
// CHECK-NEXT: }
