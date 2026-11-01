// FR-250: control flow C++ -> canonical Rust under --check-roundtrip. Pins
// else-if chains, while/break/continue, the ONE canonical counting loop
// (`for (int i = a; i < b; ++i)` <=> `for i in a..b`, size_t too), recursion,
// prototypes for forward calls, and `int main() { ...; return 0; }` <=>
// `fn main()`.
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
#include <cstdio>

bool is_odd(int n);

bool is_even(int n) {
    if (n == 0) {
        return true;
    }
    return is_odd(n - 1);
}

bool is_odd(int n) {
    if (n == 0) {
        return false;
    }
    return is_even(n - 1);
}

int classify(int x) {
    if (x < 0) {
        return -1;
    } else if (x == 0) {
        return 0;
    } else if (x < 10) {
        return 1;
    } else {
        return 2;
    }
}

int main() {
    int total = 0;
    const int limit = 12;
    for (int i = 0; i < limit; ++i) {
        if (i % 3 == 0) {
            continue;
        }
        total += classify(i - 4);
    }
    for (std::size_t k = 0; k < 4; k++) {
        std::printf("%zu ", k);
    }
    int w = 100;
    while (true) {
        w -= 7;
        if (w < 50) {
            break;
        }
    }
    std::printf("%d %d %d\n", total, w, is_even(10));
    return 0;
}

// CHECK: fn is_even(mut n: i32) -> bool {
// CHECK-NEXT:     if n == 0 {
// CHECK-NEXT:         return true;
// CHECK-NEXT:     }
// CHECK-NEXT:     return is_odd(n - 1);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn is_odd(mut n: i32) -> bool {
// CHECK-NEXT:     if n == 0 {
// CHECK-NEXT:         return false;
// CHECK-NEXT:     }
// CHECK-NEXT:     return is_even(n - 1);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn classify(mut x: i32) -> i32 {
// CHECK-NEXT:     if x < 0 {
// CHECK-NEXT:         return -1;
// CHECK-NEXT:     } else if x == 0 {
// CHECK-NEXT:         return 0;
// CHECK-NEXT:     } else if x < 10 {
// CHECK-NEXT:         return 1;
// CHECK-NEXT:     } else {
// CHECK-NEXT:         return 2;
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut total: i32 = 0;
// CHECK-NEXT:     let limit: i32 = 12;
// CHECK-NEXT:     for i in 0..limit {
// CHECK-NEXT:         if (i % 3) == 0 {
// CHECK-NEXT:             continue;
// CHECK-NEXT:         }
// CHECK-NEXT:         total += classify(i - 4);
// CHECK-NEXT:     }
// CHECK-NEXT:     for k in 0usize..4usize {
// CHECK-NEXT:         print!("{} ", k);
// CHECK-NEXT:     }
// CHECK-NEXT:     let mut w: i32 = 100;
// CHECK-NEXT:     while true {
// CHECK-NEXT:         w -= 7;
// CHECK-NEXT:         if w < 50 {
// CHECK-NEXT:             break;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT:     print!("{} {} {}\n", total, w, is_even(10) as i32);
// CHECK-NEXT: }

// CPP: #include <cstdio>
// CPP-EMPTY:
// CPP-NEXT: bool is_odd(int n);
// CPP-EMPTY:
// CPP-NEXT: bool is_even(int n) {
// CPP-NEXT:     if (n == 0) {
// CPP-NEXT:         return true;
// CPP-NEXT:     }
// CPP-NEXT:     return is_odd(n - 1);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: bool is_odd(int n) {
// CPP-NEXT:     if (n == 0) {
// CPP-NEXT:         return false;
// CPP-NEXT:     }
// CPP-NEXT:     return is_even(n - 1);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int classify(int x) {
// CPP-NEXT:     if (x < 0) {
// CPP-NEXT:         return -1;
// CPP-NEXT:     } else if (x == 0) {
// CPP-NEXT:         return 0;
// CPP-NEXT:     } else if (x < 10) {
// CPP-NEXT:         return 1;
// CPP-NEXT:     } else {
// CPP-NEXT:         return 2;
// CPP-NEXT:     }
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     int total = 0;
// CPP-NEXT:     const int limit = 12;
// CPP-NEXT:     for (int i = 0; i < limit; ++i) {
// CPP-NEXT:         if ((i % 3) == 0) {
// CPP-NEXT:             continue;
// CPP-NEXT:         }
// CPP-NEXT:         total += classify(i - 4);
// CPP-NEXT:     }
// CPP-NEXT:     for (std::size_t k = static_cast<std::size_t>(0); k < static_cast<std::size_t>(4); ++k) {
// CPP-NEXT:         std::printf("%zu ", k);
// CPP-NEXT:     }
// CPP-NEXT:     int w = 100;
// CPP-NEXT:     while (true) {
// CPP-NEXT:         w -= 7;
// CPP-NEXT:         if (w < 50) {
// CPP-NEXT:             break;
// CPP-NEXT:         }
// CPP-NEXT:     }
// CPP-NEXT:     std::printf("%d %d %d\n", total, w, static_cast<int>(is_even(10)));
// CPP-NEXT:     return 0;
// CPP-NEXT: }
