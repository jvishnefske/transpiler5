// FR-250: law 2 at the CLI, starting from Rust: the C++ the tool prints for
// this Rust, fed back in, prints byte-identical C++ (and the Rust it prints
// from that C++ is this file's canonical Rust). Pins that cxx_print and
// cxx_import agree on every spelling used here, through real clang.
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: %emitrust-bx --emit=cpp %t.cpp -o %t.2.cpp
// RUN: diff %t.cpp %t.2.cpp
// RUN: %emitrust-bx --emit=rust %t.cpp -o %t.rs
// RUN: %emitrust-bx --emit=rust %s -o %t.self.rs
// RUN: diff %t.rs %t.self.rs
fn sum_to(n: usize) -> usize {
    let mut s: usize = 0usize;
    for i in 0usize..n {
        s = s.wrapping_add(i);
    }
    return s;
}

fn main() {
    let x: f64 = 0.1 + 0.2;
    let b: bool = (x > 0.3) && !(x > 1.0);
    let c: i8 = 90i8;
    print!("{} {} {} {}\n", sum_to(10usize), bx::fmt_f(x, 17), b as i32, bx::chr(c as i32));
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

    pub fn chr(x: i32) -> char {
        let b: u8 = x as u8;
        assert!(b < 128, "bx::chr: printf %c of a non-ASCII byte is outside the subset");
        b as char
    }
}
