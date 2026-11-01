// FR-250: the printf <=> print! rows under --check-roundtrip. Pins the
// probed mapping: integer width/flags map to {:W} {:<W} {:0W}; every %f/%.Nf
// goes through the bx::fmt_f helper (glibc spells NaN "nan"/"-nan", Rust
// "NaN"); %c through bx::chr; %s only with a literal; { } escaped as {{ }};
// %i/%ld/%lu import as %d/%lld/%zu and print back canonically.
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
#include <cstdio>

int main() {
    int i = -42;
    long long l = 1234567890123LL;
    unsigned u = 7u;
    unsigned long long ul = 18446744073709551615ULL;
    std::size_t z = 99;
    double d = 2.5;
    char c = 'q';
    printf("%d|%5d|%-5d|%05d|%i\n", i, i, i, i, i);
    printf("%lld %ld %u %llu %zu %lu\n", l, l, u, ul, z, z);
    printf("%f %.0f %.2f %.6f\n", d, d, d, d);
    printf("%c %s {braces} 100%%\n", c, "str");
    return 0;
}

// CHECK: fn main() {
// CHECK-NEXT:     let mut i: i32 = -42;
// CHECK-NEXT:     let mut l: i64 = 1234567890123i64;
// CHECK-NEXT:     let mut u: u32 = 7u32;
// CHECK-NEXT:     let mut ul: u64 = 18446744073709551615u64;
// CHECK-NEXT:     let mut z: usize = 99usize;
// CHECK-NEXT:     let mut d: f64 = 2.5;
// CHECK-NEXT:     let mut c: i8 = 113i8;
// CHECK-NEXT:     print!("{}|{:5}|{:<5}|{:05}|{}\n", i, i, i, i, i);
// CHECK-NEXT:     print!("{} {} {} {} {} {}\n", l, l, u, ul, z, z);
// CHECK-NEXT:     print!("{} {} {} {}\n", bx::fmt_f(d, 6), bx::fmt_f(d, 0), bx::fmt_f(d, 2), bx::fmt_f(d, 6));
// CHECK-NEXT:     print!("{} {} {{[{][{]}}braces{{[}][}]}} 100%\n", bx::chr(c as i32), "str");
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: mod bx {
// CHECK-NEXT:     pub fn fmt_f(x: f64, prec: usize) -> String {
// CHECK-NEXT:         if x.is_nan() {
// CHECK-NEXT:             if x.is_sign_negative() {
// CHECK-NEXT:                 return String::from("-nan");
// CHECK-NEXT:             }
// CHECK-NEXT:             return String::from("nan");
// CHECK-NEXT:         }
// CHECK-NEXT:         format!("{:.*}", prec, x)
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn chr(x: i32) -> char {
// CHECK-NEXT:         let b: u8 = x as u8;
// CHECK-NEXT:         assert!(b < 128, "bx::chr: printf %c of a non-ASCII byte is outside the subset");
// CHECK-NEXT:         b as char
// CHECK-NEXT:     }
// CHECK-NEXT: }

// CPP: #include <cstdio>
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     int i = -42;
// CPP-NEXT:     long long l = 1234567890123LL;
// CPP-NEXT:     unsigned u = 7U;
// CPP-NEXT:     unsigned long long ul = 18446744073709551615ULL;
// CPP-NEXT:     std::size_t z = static_cast<std::size_t>(99);
// CPP-NEXT:     double d = 2.5;
// CPP-NEXT:     char c = 'q';
// CPP-NEXT:     std::printf("%d|%5d|%-5d|%05d|%d\n", i, i, i, i, i);
// CPP-NEXT:     std::printf("%lld %lld %u %llu %zu %zu\n", l, l, u, ul, z, z);
// CPP-NEXT:     std::printf("%f %.0f %.2f %f\n", d, d, d, d);
// CPP-NEXT:     std::printf("%c %s {braces} 100%%\n", static_cast<int>(c), "str");
// CPP-NEXT:     return 0;
// CPP-NEXT: }
