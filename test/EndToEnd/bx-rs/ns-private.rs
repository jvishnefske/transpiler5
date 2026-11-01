// REQUIRES: cargo
// FR-260: emitrust-bx Rust -> C++, private module items and the safe helpers
// inside modules end to end (rustc vs clang++ byte-diff). A private `fn` in
// `mod text` is a member of an anonymous namespace inside `namespace text`,
// called bare from its own module and as `crate::text::pad` from the nested
// `text::fancy` (allowed: a descendant). Inside a module the helpers are
// spelled `crate::bx::substr` / `crate::bx::push_byte` / `crate::bx::fmt_f`
// / `crate::bx::chr`, <=> `s.substr`, `push_back`, `%.Nf`, `%c` in C++.
// Distinct values per call site; ASCII only; UB-free.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
mod text {
    fn pad(s: &String, n: usize) -> String {
        let mut out: String = s.clone();
        while out.len() < n {
            crate::bx::push_byte(&mut out, 46i8);
        }
        return out;
    }

    pub fn head(s: &String, n: usize) -> String {
        return pad(&crate::bx::substr(s, 0usize, n), n.wrapping_add(2usize));
    }

    pub fn show(x: f64, c: i32) {
        print!("[{}] [{}]\n", crate::bx::fmt_f(x, 3), crate::bx::chr(c));
    }

    pub mod fancy {
        pub fn frame(s: &String) -> String {
            return String::from("<") + &crate::text::pad(s, 6usize) + ">";
        }
    }
}

fn main() {
    let w: String = String::from("namespace");
    print!("{} {}\n", text::head(&w, 4usize), crate::text::head(&w, 1usize));
    print!("{} {}\n", text::fancy::frame(&String::from("ab")), crate::text::fancy::frame(&w));
    text::show(2.0625, 81);
    crate::text::show(-0.5, 104);
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

    pub fn push_byte(s: &mut String, c: i8) {
        let b: u8 = c as u8;
        assert!(b < 128, "bx::push_byte: a byte >= 0x80 would break the String ASCII invariant");
        s.push(b as char);
    }

    pub fn substr(s: &str, pos: usize, n: usize) -> String {
        assert!(pos <= s.len(), "bx::substr: position past the end (std::out_of_range in C++)");
        let end: usize = pos + std::cmp::min(n, s.len() - pos);
        String::from(&s[pos..end])
    }
}
