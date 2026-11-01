// FR-254: the first normalizer rules under --normalize, C++ -> canonical
// subset C++ and Rust. R1 rewrites every `for` outside the canonical counting
// form to `{ I; while (C) { B S; } }` (a missing C is `true`; the block keeps
// I's names scoped to the loop); R2 rewrites `do B while (C);` to
// `while (true) { B if (!(C)) { break; } }`. Pinned: the canonical outputs of
// both languages, the located `note: normalized:` per applied rule, laws 1-2
// on the OUTPUT (--check-roundtrip re-imports the printed C++ WITHOUT
// --normalize), the cpp -> cpp fixed point, and `shadowed_step`, whose step
// `i += t` names the OUTER `t` while the body declares its own -- the body
// stays a nested block so splicing cannot capture it.
// RUN: %emitrust-bx --normalize --check-roundtrip %s 2> %t.notes | FileCheck %s
// RUN: FileCheck --check-prefix=NOTE --input-file=%t.notes %s
// RUN: %emitrust-bx --normalize --emit=cpp %s -o %t.cpp 2> /dev/null
// RUN: FileCheck --check-prefix=CPP --input-file=%t.cpp %s
// RUN: %emitrust-bx --emit=cpp %t.cpp | diff %t.cpp -
// RUN: not %emitrust-bx %s 2>&1 | FileCheck --check-prefix=OFF %s
// NOTE: normalize-loops.cpp:29:5: note: normalized: for->while
// NOTE-NEXT: normalize-loops.cpp:37:5: note: normalized: for->while
// NOTE-NEXT: normalize-loops.cpp:48:5: note: normalized: for->while
// NOTE-NEXT: normalize-loops.cpp:55:5: note: normalized: for->while
// NOTE-NEXT: normalize-loops.cpp:66:5: note: normalized: for->while
// NOTE-NEXT: normalize-loops.cpp:75:5: note: normalized: do-while
// OFF: normalize-loops.cpp:29:5: error: unsupported: `for` loop outside the canonical counting form
// OFF: normalize-loops.cpp:75:5: error: unsupported: do-while loop
#include <cstdio>

int strided(int n) {
    int s = 0;
    for (int i = 1; i < n; i += 4) {
        s += i;
    }
    return s;
}

int countdown(int n) {
    int s = 0;
    for (int i = n; i != 0; --i) {
        if (s > 100) {
            break;
        }
        s += i;
    }
    return s;
}

unsigned varint_bytes(unsigned v) {
    unsigned bytes = 1U;
    for (; v >= 128U; v >>= 7) {
        bytes += 1U;
    }
    return bytes;
}

int first_power_above(int lim) {
    for (int c = 1;; c *= 2) {
        if (c > lim) {
            return c;
        }
    }
    return 0;
}

int shadowed_step(int n) {
    int s = 0;
    int t = 2;
    for (int i = 0; i < n; i += t) {
        int t = i % 3;
        s += t;
    }
    return s;
}

int digits(int n) {
    int count = 0;
    do {
        count += 1;
        n /= 10;
    } while (n != 0);
    return count;
}

int main() {
    std::printf("%d %d %u %d %d %d\n", strided(20), countdown(30), varint_bytes(300000U),
                first_power_above(70), shadowed_step(11), digits(0));
    return 0;
}

// CHECK: fn strided(mut n: i32) -> i32 {
// CHECK-NEXT:     let mut s: i32 = 0;
// CHECK-NEXT:     {
// CHECK-NEXT:         let mut i: i32 = 1;
// CHECK-NEXT:         while i < n {
// CHECK-NEXT:             s += i;
// CHECK-NEXT:             i += 4;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT:     return s;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn countdown(mut n: i32) -> i32 {
// CHECK-NEXT:     let mut s: i32 = 0;
// CHECK-NEXT:     {
// CHECK-NEXT:         let mut i: i32 = n;
// CHECK-NEXT:         while i != 0 {
// CHECK-NEXT:             if s > 100 {
// CHECK-NEXT:                 break;
// CHECK-NEXT:             }
// CHECK-NEXT:             s += i;
// CHECK-NEXT:             i -= 1;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT:     return s;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn varint_bytes(mut v: u32) -> u32 {
// CHECK-NEXT:     let mut bytes: u32 = 1u32;
// CHECK-NEXT:     while v >= 128u32 {
// CHECK-NEXT:         bytes = bytes.wrapping_add(1u32);
// CHECK-NEXT:         v >>= 7;
// CHECK-NEXT:     }
// CHECK-NEXT:     return bytes;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn first_power_above(mut lim: i32) -> i32 {
// CHECK-NEXT:     {
// CHECK-NEXT:         let mut c: i32 = 1;
// CHECK-NEXT:         while true {
// CHECK-NEXT:             if c > lim {
// CHECK-NEXT:                 return c;
// CHECK-NEXT:             }
// CHECK-NEXT:             c *= 2;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT:     return 0;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn shadowed_step(mut n: i32) -> i32 {
// CHECK-NEXT:     let mut s: i32 = 0;
// CHECK-NEXT:     let mut t: i32 = 2;
// CHECK-NEXT:     {
// CHECK-NEXT:         let mut i: i32 = 0;
// CHECK-NEXT:         while i < n {
// CHECK-NEXT:             {
// CHECK-NEXT:                 let mut t: i32 = i % 3;
// CHECK-NEXT:                 s += t;
// CHECK-NEXT:             }
// CHECK-NEXT:             i += t;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT:     return s;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn digits(mut n: i32) -> i32 {
// CHECK-NEXT:     let mut count: i32 = 0;
// CHECK-NEXT:     while true {
// CHECK-NEXT:         count += 1;
// CHECK-NEXT:         n /= 10;
// CHECK-NEXT:         if !(n != 0) {
// CHECK-NEXT:             break;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT:     return count;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     print!("{} {} {} {} {} {}\n", strided(20), countdown(30), varint_bytes(300000u32), first_power_above(70), shadowed_step(11), digits(0));
// CHECK-NEXT: }

// CPP: #include <cstdio>
// CPP-EMPTY:
// CPP-NEXT: int strided(int n) {
// CPP-NEXT:     int s = 0;
// CPP-NEXT:     {
// CPP-NEXT:         int i = 1;
// CPP-NEXT:         while (i < n) {
// CPP-NEXT:             s += i;
// CPP-NEXT:             i += 4;
// CPP-NEXT:         }
// CPP-NEXT:     }
// CPP-NEXT:     return s;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int countdown(int n) {
// CPP-NEXT:     int s = 0;
// CPP-NEXT:     {
// CPP-NEXT:         int i = n;
// CPP-NEXT:         while (i != 0) {
// CPP-NEXT:             if (s > 100) {
// CPP-NEXT:                 break;
// CPP-NEXT:             }
// CPP-NEXT:             s += i;
// CPP-NEXT:             --i;
// CPP-NEXT:         }
// CPP-NEXT:     }
// CPP-NEXT:     return s;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: unsigned varint_bytes(unsigned v) {
// CPP-NEXT:     unsigned bytes = 1U;
// CPP-NEXT:     while (v >= 128U) {
// CPP-NEXT:         ++bytes;
// CPP-NEXT:         v >>= 7;
// CPP-NEXT:     }
// CPP-NEXT:     return bytes;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int first_power_above(int lim) {
// CPP-NEXT:     {
// CPP-NEXT:         int c = 1;
// CPP-NEXT:         while (true) {
// CPP-NEXT:             if (c > lim) {
// CPP-NEXT:                 return c;
// CPP-NEXT:             }
// CPP-NEXT:             c *= 2;
// CPP-NEXT:         }
// CPP-NEXT:     }
// CPP-NEXT:     return 0;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int shadowed_step(int n) {
// CPP-NEXT:     int s = 0;
// CPP-NEXT:     int t = 2;
// CPP-NEXT:     {
// CPP-NEXT:         int i = 0;
// CPP-NEXT:         while (i < n) {
// CPP-NEXT:             {
// CPP-NEXT:                 int t = i % 3;
// CPP-NEXT:                 s += t;
// CPP-NEXT:             }
// CPP-NEXT:             i += t;
// CPP-NEXT:         }
// CPP-NEXT:     }
// CPP-NEXT:     return s;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int digits(int n) {
// CPP-NEXT:     int count = 0;
// CPP-NEXT:     while (true) {
// CPP-NEXT:         ++count;
// CPP-NEXT:         n /= 10;
// CPP-NEXT:         if (!(n != 0)) {
// CPP-NEXT:             break;
// CPP-NEXT:         }
// CPP-NEXT:     }
// CPP-NEXT:     return count;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     std::printf("%d %d %u %d %d %d\n", strided(20), countdown(30), varint_bytes(300000U), first_power_above(70), shadowed_step(11), digits(0));
// CPP-NEXT:     return 0;
// CPP-NEXT: }
