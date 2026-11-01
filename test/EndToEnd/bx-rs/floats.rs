// REQUIRES: cargo
// FR-250: emitrust-bx Rust -> C++, f64 and the printf float mapping end to
// end (rustc vs clang++/glibc byte-diff). Pins: bx::fmt_f(x, N) <=> %.Nf
// (N = 6 prints as %f) on exact ties, -0.0, a run-time inf, and long
// series whose trailing digits expose any precision change; `as` between
// f64 and i32/i64 (truncation, in range). NaN is not printed: its sign out
// of a run-time vs folded 0/0 is unspecified (see bx-printf.cpp).
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn harmonic(n: i32) -> f64 {
    let mut s: f64 = 0.0;
    for k in 1..n {
        s += 1.0 / (k as f64);
    }
    return s;
}

fn scale(x: f64, times: i32) -> f64 {
    let mut y: f64 = x;
    for i in 0..times {
        y = y * 100.0;
    }
    return y;
}

fn main() {
    print!("{} {} {}\n", bx::fmt_f(harmonic(10), 6), bx::fmt_f(harmonic(1000), 12), bx::fmt_f(harmonic(77), 3));
    print!("{} {} {} {}\n", bx::fmt_f(0.125, 2), bx::fmt_f(2.5, 0), bx::fmt_f(3.5, 0), bx::fmt_f(-0.0, 1));
    let inf: f64 = scale(1e+300, 5);
    print!("{} {}\n", bx::fmt_f(inf, 2), bx::fmt_f(-inf, 0));
    let t: f64 = -7.9;
    print!("{} {} {}\n", t as i32, (t * 1000000000.0) as i64, (((t as i32) as f64) / 4.0 > -2.0) as i32);
    print!("{}|{:6}|{:<6}|\n", bx::fmt_f(1.0 / 7.0, 10), (harmonic(5) * 10.0) as i32, -3);
}

mod bx {
    pub fn fmt_f(x: f64, prec: usize) -> String {
        if x.is_nan() {
            if x.is_sign_negative() {
                return String::from("-nan");
            }
            return String::from("nan");
        }
        format!("{:.*}", prec, x)
    }
}
