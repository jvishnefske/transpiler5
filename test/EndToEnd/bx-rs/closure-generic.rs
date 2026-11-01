// REQUIRES: cargo
// FR-252: emitrust-bx Rust -> C++, callable PARAMETERS end to end (rustc vs
// clang++ byte-diff). `f: &impl Fn(..) -> R` <=> `template <typename F> ..
// const F& f`, `f: &mut impl FnMut(..)` <=> `F& f`; `g(&add, ..)` /
// `g(&mut bump, ..)` <=> `g(add, ..)` / `g(bump, ..)`. The state of a `move`
// closure advanced through `&mut impl FnMut` is the CALLER's state afterwards
// (C++ `F&` binds the caller's closure object; a by-value `F` would copy it);
// a callable parameter passed on to another function, one used inside a
// loop, functions in a module called by absolute path, and one closure handed
// to two functions. Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn sum_map(f: &impl Fn(i32) -> i64, n: i32) -> i64 {
    let mut t: i64 = 0i64;
    for i in 0..n {
        t += f(i);
    }
    return t;
}

fn drive(f: &mut impl FnMut(i32) -> i32, times: i32) -> i32 {
    let mut last: i32 = 0;
    for i in 0..times {
        last = f(i * 3);
    }
    return last;
}

fn drive_twice(f: &mut impl FnMut(i32) -> i32) -> i32 {
    let a: i32 = drive(f, 2);
    let b: i32 = drive(f, 1);
    return a * 1000 + b;
}

mod calc {
    pub fn pick(f: &impl Fn(i32, i32) -> bool, a: i32, b: i32) -> i32 {
        if f(a, b) {
            return a;
        }
        return b;
    }
}

fn main() {
    let base: i64 = 5i64;
    let sq = move |x: i32| -> i64 {
        return (x as i64) * (x as i64) + base;
    };
    let s1: i64 = sum_map(&sq, 4);
    let s2: i64 = sum_map(&sq, 7);
    let mut seen: i32 = 1;
    let mut acc = move |x: i32| -> i32 {
        seen = seen * 2 + x;
        return seen;
    };
    let d1: i32 = drive(&mut acc, 3);
    let d2: i32 = drive_twice(&mut acc);
    let d3: i32 = acc(100);
    seen = -1;
    let mut log: i32 = 0;
    let mut count = |x: i32| -> i32 {
        log = log * 10 + 1;
        return x + log;
    };
    let c1: i32 = drive_twice(&mut count);
    let less = |a: i32, b: i32| -> bool {
        return a < b;
    };
    let more = |a: i32, b: i32| -> bool {
        return a > b;
    };
    let p1: i32 = crate::calc::pick(&less, 4, 9);
    let p2: i32 = crate::calc::pick(&more, 4, 9);
    print!("{} {} {} {} {} {} {} {} {} {}\n", s1, s2, d1, d2, d3, seen, c1, log, p1, p2);
}
