// REQUIRES: cargo
// FR-263, the Rust side: statements whose C++ spelling still has ONE legal
// output stay in the subset -- a printing call nested in another call's
// arguments, printing calls as the operands of `&&` and of an `if`
// expression (C++ `?:`), one printing call beside pure ones. Byte-diffed
// rustc vs clang++ (g++ agrees: checked by hand).
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn g(x: i32) -> i32 {
    print!("g{} ", x);
    return x;
}

fn sq(x: i32) -> i32 {
    return x * x;
}

fn outer(a: i32, b: i32) -> i32 {
    print!("outer{},{} ", a, b);
    return a + b;
}

fn main() {
    let a: i32 = outer(g(1), sq(2) + sq(3));
    let mut c: i32 = 0;
    if (g(4) > 3) && (g(5) > 9) {
        c = 1;
    }
    let d: i32 = if g(6) > 0 { g(7) } else { g(8) };
    let e: i32 = g(9) * sq(3);
    print!("\n{} {} {} {}\n", a, c, d, e);
}
