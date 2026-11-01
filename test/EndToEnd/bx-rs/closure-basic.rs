// REQUIRES: cargo
// FR-252: emitrust-bx Rust -> C++, closures end to end (rustc vs clang++
// byte-diff). A `move` closure <=> by-copy captures `[k, s]`, a non-move one
// <=> by-reference captures `[&acc]`. The load-bearing row: a variable
// reassigned AFTER a `move` closure is made and before it is called -- the
// closure keeps its copy (C++ must print `[k]`, not `[&k]`). Also closures
// with no capture, typed parameters (`mut a: i32` <=> `int a`, `a: i32` <=>
// `const int a`), every scalar result, a closure that returns early from a
// loop, closures made inside a counting loop over the loop variable, and a
// by-reference closure reading a vector it does not own. Distinct values at
// every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn snapshot(seed: i32) -> i32 {
    let mut k: i32 = seed;
    let snap = move |a: i32| -> i32 {
        return a * 7 + k;
    };
    k = k * 5;
    let x: i32 = snap(2);
    k += 3;
    let y: i32 = snap(3);
    print!("snapshot: x={} y={} k={}\n", x, y, k);
    return x + y + k;
}

fn kinds(n: i64) -> i64 {
    let half = |x: i64| -> f64 {
        return (x as f64) / 2.0;
    };
    let is_big = move |x: i64| -> bool {
        return x > n;
    };
    let letter = |i: i32| -> i8 {
        return (97i32 + i) as i8;
    };
    let wide = |mut u: u64| -> u64 {
        u = u.wrapping_mul(3u64);
        return u.wrapping_add(1u64);
    };
    let h: f64 = half(n + 3i64);
    let big: i32 = if is_big(n + 1i64) { 1 } else { 0 };
    let small: i32 = if is_big(n - 1i64) { 1 } else { 0 };
    print!("kinds: {} {} {} {} {}\n", (h * 100.0) as i64, big, small, letter(2) as i32,
           wide(18446744073709551615u64));
    return (half(n) * 10.0) as i64;
}

fn search(v: &Vec<i32>, want: i32) -> i32 {
    let find = |w: i32| -> i32 {
        let mut i: usize = 0usize;
        while i < v.len() {
            if v[i] == w {
                return i as i32;
            }
            i = i.wrapping_add(1usize);
        }
        return -1;
    };
    let a: i32 = find(want);
    let b: i32 = find(want + 1);
    print!("search: {} {}\n", a, b);
    return a * 10 + b;
}

fn grid(n: i32) -> i32 {
    let mut total: i32 = 0;
    for i in 0..n {
        let row = move |j: i32| -> i32 {
            return i * 10 + j;
        };
        let mut add = |x: i32| {
            total += x;
        };
        add(row(i + 1));
        add(row(2));
    }
    print!("grid: {}\n", total);
    return total;
}

fn main() {
    let a: i32 = snapshot(4);
    let b: i64 = kinds(9i64);
    let v: Vec<i32> = vec![5, 8, 13, 9];
    let c: i32 = search(&v, 8);
    let d: i32 = grid(4);
    print!("main: {} {} {} {}\n", a, b, c, d);
}

