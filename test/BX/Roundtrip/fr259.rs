// FR-259: Rust -> C++, law 2 at the CLI from the Rust side (the C++ printed
// for this file, fed back in, prints byte-identical C++; its Rust is this
// file's canonical Rust). Pins that rs_parse reads an `if c { a } else { b }`
// EXPRESSION (an `else if` chain, an argument, a let value, a cast operand) as
// `c ? a : (..)`, the canonical increment blocks `{ k -= 1; k + 1 }`, `{ *n =
// (*n).wrapping_add(1u32); (*n).wrapping_sub(1u32) }`, `{ i =
// i.wrapping_add(1usize); i }` as `k--`, `n++`, `++i` (in an index, a `while`
// condition, a return), and `i16` / `u16` as `short` / `unsigned short`.
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: %emitrust-bx --emit=cpp %t.cpp -o %t.2.cpp
// RUN: diff %t.cpp %t.2.cpp
// RUN: %emitrust-bx --emit=rust %t.cpp -o %t.rs
// RUN: %emitrust-bx --emit=rust %s -o %t.self.rs
// RUN: diff %t.rs %t.self.rs
// RUN: %emitrust-bx --check-roundtrip --emit=rust %s | FileCheck %s
// RUN: FileCheck --check-prefix=CPP %s --input-file=%t.cpp
// CHECK:      fn clamp(x: i64, lo: i64, hi: i64) -> i64 {
// CHECK-NEXT:     return if x < lo { lo } else if x > hi { hi } else { x };
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn take(n: &mut u32) -> u32 {
// CHECK-NEXT:     return { *n = (*n).wrapping_add(1u32); (*n).wrapping_sub(1u32) };
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn widen(a: i16, b: u16) -> i32 {
// CHECK-NEXT:     let s: i16 = (((a as i32) * 3i32) + (b as i32)) as i16;
// CHECK-NEXT:     let t: u16 = ((b as i32) + 40000i32) as u16;
// CHECK-NEXT:     return (s as i32) + (t as i32);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut k: i32 = 4;
// CHECK-NEXT:     let mut total: i64 = 0i64;
// CHECK-NEXT:     while ({ k -= 1; k + 1 }) > 0 {
// CHECK-NEXT:         total = (total * 3i64) + clamp(((k as i64) * 5i64) - 9i64, -4i64, 20i64);
// CHECK-NEXT:     }
// CHECK-NEXT:     let mut c: u32 = 4294967294u32;
// CHECK-NEXT:     let a: u32 = take(&mut c);
// CHECK-NEXT:     let mut i: usize = 0usize;
// CHECK-NEXT:     let mut v: Vec<i32> = vec![0; 4usize];
// CHECK-NEXT:     v[{ i = i.wrapping_add(1usize); i.wrapping_sub(1usize) }] = 11;
// CHECK-NEXT:     v[{ i = i.wrapping_add(1usize); i }] = 13;
// CHECK-NEXT:     let pick: i32 = if total > 10i64 { v[0usize] } else { v[2usize] };
// CHECK-NEXT:     print!("{} {} {} {} {}\n", total, a, c, pick, if pick > 0 { widen((-12345i32) as i16, 65000u16) } else { 0 });
// CHECK-NEXT: }
// CPP:      #include <cstdio>
// CPP-NEXT: #include <vector>
// CPP-EMPTY:
// CPP-NEXT: long long clamp(const long long x, const long long lo, const long long hi) {
// CPP-NEXT:     return x < lo ? lo : (x > hi ? hi : x);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: unsigned take(unsigned& n) {
// CPP-NEXT:     return n++;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int widen(const short a, const unsigned short b) {
// CPP-NEXT:     const short s = static_cast<short>((static_cast<int>(a) * 3) + static_cast<int>(b));
// CPP-NEXT:     const unsigned short t = static_cast<unsigned short>(static_cast<int>(b) + 40000);
// CPP-NEXT:     return static_cast<int>(s) + static_cast<int>(t);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     int k = 4;
// CPP-NEXT:     long long total = 0LL;
// CPP-NEXT:     while (k-- > 0) {
// CPP-NEXT:         total = (total * 3LL) + clamp((static_cast<long long>(k) * 5LL) - 9LL, -4LL, 20LL);
// CPP-NEXT:     }
// CPP-NEXT:     unsigned c = 4294967294U;
// CPP-NEXT:     const unsigned a = take(c);
// CPP-NEXT:     std::size_t i = static_cast<std::size_t>(0);
// CPP-NEXT:     std::vector<int> v(static_cast<std::size_t>(4), 0);
// CPP-NEXT:     v[i++] = 11;
// CPP-NEXT:     v[++i] = 13;
// CPP-NEXT:     const int pick = total > 10LL ? v[static_cast<std::size_t>(0)] : v[static_cast<std::size_t>(2)];
// CPP-NEXT:     std::printf("%lld %u %u %d %d\n", total, a, c, pick, pick > 0 ? widen(static_cast<short>(-12345), static_cast<unsigned short>(65000)) : 0);
// CPP-NEXT:     return 0;
// CPP-NEXT: }
fn clamp(x: i64, lo: i64, hi: i64) -> i64 {
    return if x < lo { lo } else if x > hi { hi } else { x };
}

fn take(n: &mut u32) -> u32 {
    return { *n = (*n).wrapping_add(1u32); (*n).wrapping_sub(1u32) };
}

fn widen(a: i16, b: u16) -> i32 {
    let s: i16 = (((a as i32) * 3i32) + (b as i32)) as i16;
    let t: u16 = ((b as i32) + 40000i32) as u16;
    return (s as i32) + (t as i32);
}

fn main() {
    let mut k: i32 = 4;
    let mut total: i64 = 0i64;
    while ({ k -= 1; k + 1 }) > 0 {
        total = (total * 3i64) + clamp(((k as i64) * 5i64) - 9i64, -4i64, 20i64);
    }
    let mut c: u32 = 4294967294u32;
    let a: u32 = take(&mut c);
    let mut i: usize = 0usize;
    let mut v: Vec<i32> = vec![0; 4usize];
    v[{ i = i.wrapping_add(1usize); i.wrapping_sub(1usize) }] = 11;
    v[{ i = i.wrapping_add(1usize); i }] = 13;
    let pick: i32 = if total > 10i64 { v[0usize] } else { v[2usize] };
    println!("{} {} {} {} {}", total, a, c, pick, (if pick > 0 { widen((-12345i32) as i16, 65000u16) } else { 0 }));
}
