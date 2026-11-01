// REQUIRES: cargo
// FR-251: emitrust-bx Rust -> C++, references and iteration end to end
// (rustc vs clang++ byte-diff). Pins `&Vec<T>` / `&mut Vec<T>` / `&String`
// / `&mut String` / `&mut i32` / `&u64` parameters <=> `const T&` / `T&`,
// `*x` for a scalar reference in value position and as an assignment target,
// call sites `f(&v)` / `f(&mut v)` / a reference forwarded bare / an element
// `&mut v[i]`, `for x in v.iter()` / `.iter_mut()` / `.iter().copied()` /
// `for mut x in .iter().cloned()` <=> the four C++ range-for forms, and
// `.clone()` <=> a C++ copy (a by-value argument, `return p.clone()` of a
// reference parameter). Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn incr(x: &mut i32, by: &u64) {
    *x = *x + (*by as i32);
}

fn scale(v: &mut Vec<i32>, k: i32) {
    for x in v.iter_mut() {
        *x *= k;
    }
}

fn sum(v: &Vec<i32>) -> i64 {
    let mut s: i64 = 0i64;
    for x in v.iter() {
        s += *x as i64;
    }
    return s;
}

fn scale_and_sum(v: &mut Vec<i32>, k: i32) -> i64 {
    scale(v, k);
    return sum(v);
}

fn copy_of(v: &Vec<i32>) -> Vec<i32> {
    return v.clone();
}

fn consume(mut v: Vec<i32>, extra: i32) -> i64 {
    v.push(extra);
    return sum(&v);
}

fn wrap(s: &mut String, left: &String) {
    let mut t: String = left.clone();
    t.push_str(s);
    t.push_str(left);
    *s = t;
}

fn count_long(words: &Vec<String>, min: usize) -> i32 {
    let mut n: i32 = 0;
    for w in words.iter() {
        if w.len() >= min {
            n += 1;
        }
    }
    return n;
}

fn main() {
    let mut a: i32 = 5;
    let by: u64 = 37u64;
    incr(&mut a, &by);
    incr(&mut a, &1000u64);
    print!("a={}\n", a);
    let mut v: Vec<i32> = vec![2, -7, 11];
    print!("{}\n", sum(&v));
    let scaled: i64 = scale_and_sum(&mut v, 3);
    print!("{}\n", scaled);
    incr(&mut v[1usize], &by);
    let snap: Vec<i32> = copy_of(&v);
    scale(&mut v, -1);
    print!("{} {} {}\n", sum(&v), sum(&snap), consume(v.clone(), 400));
    let mut total: i32 = 0;
    for mut x in snap.iter().copied() {
        x = x * x;
        total += x;
    }
    print!("total={} snap0={}\n", total, snap[0usize]);
    let mut name: String = String::from("mid");
    let edge: String = String::from("**");
    wrap(&mut name, &edge);
    wrap(&mut name, &String::from("-"));
    print!("{} {}\n", name, name.len());
    let words: Vec<String> = vec![String::from("a"), name.clone(), String::from("tree"), edge.clone()];
    print!("{} {}\n", count_long(&words, 2usize), count_long(&words, 5usize));
    let mut out: String = String::new();
    for mut w in words.iter().cloned() {
        w.push_str(".");
        out.push_str(&w);
    }
    print!("{}\n", out);
}
