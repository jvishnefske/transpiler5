// REQUIRES: cargo
// FR-259: emitrust-bx Rust -> C++, if-EXPRESSIONS end to end (rustc vs
// clang++ byte-diff). `if c { a } else { b }` <=> `c ? a : b` as a let value,
// a return value (an `else if` chain <=> a parenthesized nested `?:`), a call
// argument, an index, a cast operand (`(if ..) as i64`, its literals
// suffixed), a binary operand, an `if` condition and a print! argument; arms
// of every scalar kind (i32, i64, u32, f64, bool, i8). Laziness: each arm
// calls a function that bumps its own counter through `&mut`, so evaluating
// the wrong arm, or both, changes the output. Distinct values at every call
// site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn bump(counter: &mut i32, v: i32) -> i32 {
    *counter += 1;
    return v * 3;
}

fn sign(x: i64) -> i32 {
    return if x < 0i64 { -1 } else if x == 0i64 { 0 } else { 1 };
}

fn lazy(c: bool, x: i32) -> i32 {
    let mut left: i32 = 0;
    let mut right: i32 = 0;
    let a: i32 = if c { bump(&mut left, x) } else { bump(&mut right, x + 1) };
    return a + (left * 1000) + (right * 100);
}

fn mix(c: bool, u: u32, d: f64, ch: i8) -> f64 {
    let w: u32 = if c { u.wrapping_mul(3u32) } else { u.wrapping_sub(7u32) };
    let k: i64 = (if c { 300i32 } else { -5i32 }) as i64;
    let e: f64 = if w > 100u32 { d * 2.0 } else { d };
    let b: bool = if (ch as i32) > 64 { true } else { k < 0i64 };
    let z: i8 = if b { ch } else { 33i8 };
    return e + (k as f64) + (if b { 0.5 } else { 0.25 }) + ((z as i32) as f64);
}

fn main() {
    let v: Vec<i32> = vec![4, 9, 16];
    let n: usize = 2usize;
    print!("{} {} {}\n", sign(-12i64), sign(0i64), sign(77i64));
    print!("{} {} {}\n", lazy(true, 5), lazy(false, 8), lazy(true, -2));
    print!("{} {}\n", (mix(true, 40u32, 1.5, 70i8) * 1000.0) as i64, (mix(false, 3u32, -2.25, 10i8) * 1000.0) as i64);
    let pick: i32 = v[if n > 1usize { 2usize } else { 0usize }];
    let mut s: i32 = 0;
    if if pick > 10 { v[0usize] > 3 } else { false } {
        s += 100;
    }
    s += 7 * (if s > 50 { 2 } else { 3 });
    print!("{} {} {}\n", pick, s, if pick % 2 == 0 { pick / 2 } else { (3 * pick) + 1 });
}
