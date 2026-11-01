// REQUIRES: cargo
// FR-250: emitrust-bx Rust -> C++, end to end. The rustc-built binary and
// the clang++-built binary of the emitted C++ must print byte-identical
// output. Pins: trial-division loops (`for d in 2..n` with a VARIABLE bound
// the body never assigns, so C++ re-evaluating it agrees with Rust's
// once-evaluated range), `while` with an early `return`, i64 `%`, bool
// returns printed through `as i32`, and immutable `let` <=> `const`.
// Distinct values at every call site. UB-free on the C++ side (no signed
// overflow, no division by zero).
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn is_prime(n: i64) -> bool {
    if n < 2i64 {
        return false;
    }
    let mut d: i64 = 2i64;
    while d * d <= n {
        if n % d == 0i64 {
            return false;
        }
        d += 1i64;
    }
    return true;
}

fn count_below(limit: i32) -> i32 {
    let mut count: i32 = 0;
    for k in 0..limit {
        if is_prime(k as i64) {
            count += 1;
        }
    }
    return count;
}

fn nth_prime(n: i32) -> i64 {
    let mut found: i32 = 0;
    let mut cand: i64 = 1i64;
    while found < n {
        cand += 1i64;
        if is_prime(cand) {
            found += 1;
        }
    }
    return cand;
}

fn main() {
    for p in 0..50 {
        if is_prime(p as i64) {
            print!("{} ", p);
        }
    }
    print!("\n");
    print!("{} {} {}\n", count_below(100), count_below(1000), count_below(7919));
    print!("{} {} {}\n", nth_prime(1), nth_prime(25), nth_prime(500));
    print!("{} {}\n", is_prime(1000000007i64) as i32, is_prime(999999999i64) as i32);
}
