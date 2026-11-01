// REQUIRES: cargo
// FR-252: emitrust-bx Rust -> C++, closures that MODIFY STATE end to end
// (rustc vs clang++ byte-diff). `let mut f = move || { k += 1; .. }` <=> a
// C++ `mutable` lambda over its own copy (the outer `k` must stay put), `let
// mut f = || { acc += x; }` <=> a by-reference capture `[&acc]` (the outer
// variable sees every call once `f` is dead), a closure pushing to a vector
// it borrows mutably, one appending to a String, and one writing through a
// `&mut` struct method. Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
#[derive(Clone)]
struct Meter {
    pub ticks: i32,
    pub peak: i32,
}

impl Meter {
    pub fn record(&mut self, x: i32) {
        self.ticks += 1;
        if x > self.peak {
            self.peak = x;
        }
    }
}

fn own_state(seed: i32) -> i32 {
    let mut k: i32 = seed;
    let mut next = move || -> i32 {
        k = k * 3 + 1;
        return k;
    };
    let a: i32 = next();
    let b: i32 = next();
    k = 0;
    let c: i32 = next();
    print!("own_state: {} {} {} k={}\n", a, b, c, k);
    return a + b + c + k;
}

fn shared(seed: i32) -> i32 {
    let mut acc: i32 = seed;
    let mut add = |x: i32| {
        acc = acc * 2 + x;
    };
    add(1);
    add(10);
    add(100);
    print!("shared: {}\n", acc);
    return acc;
}

fn collect(n: i32) -> usize {
    let mut v: Vec<i32> = Vec::new();
    let mut s: String = String::from("<");
    let mut push = |x: i32| -> usize {
        v.push(x * x);
        return v.len();
    };
    let mut i: i32 = 0;
    while push(i + n) < 4usize {
        i += 1;
    }
    let mut tag = |t: i32| {
        if t % 2 == 0 {
            s.push_str("e");
        } else {
            s.push_str("o");
        }
    };
    tag(3);
    tag(8);
    tag(5);
    print!("collect: {} {} {} {}\n", v.len(), v[0usize], v[3usize], s);
    return v.len();
}

fn meter(a: i32, b: i32) -> i32 {
    let mut m: Meter = Meter { ticks: 0, peak: -100 };
    let mut feed = |x: i32| {
        m.record(x);
        m.record(x - 50);
    };
    feed(a);
    feed(b);
    print!("meter: ticks={} peak={}\n", m.ticks, m.peak);
    return m.peak;
}

fn main() {
    let a: i32 = own_state(2);
    let b: i32 = shared(7);
    let c: usize = collect(3);
    let d: i32 = meter(12, 90);
    print!("main: {} {} {} {}\n", a, b, c, d);
}
