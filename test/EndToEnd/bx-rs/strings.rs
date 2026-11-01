// REQUIRES: cargo
// FR-251: emitrust-bx Rust -> C++, the String rows end to end (rustc vs
// clang++ byte-diff). Pins String::new / String::from <=> std::string,
// `a.clone() + &b` / `+ "lit"` <=> `a + b`, push_str <=> `+=`,
// bx::push_byte <=> push_back (the helper PANICS on a byte >= 0x80: every
// String stays ASCII), `s.as_bytes()[i] as i8` <=> `s[i]` (signed char),
// bx::substr / bx::substr_from <=> substr(p, n) / substr(p) with the n
// clamp exercised, comparisons, `x.to_string()` <=> std::to_string, and a
// tokenizer that splits on spaces. Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn split(s: &String) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut start: usize = 0usize;
    for i in 0usize..s.len() {
        if ((s.as_bytes()[i] as i8) as i32) == 32 {
            if i > start {
                out.push(bx::substr(s, start, i.wrapping_sub(start)));
            }
            start = i.wrapping_add(1usize);
        }
    }
    if start < s.len() {
        out.push(bx::substr_from(s, start));
    }
    return out;
}

fn reversed(s: &String) -> String {
    let mut out: String = String::new();
    let mut i: usize = s.len();
    while i > 0usize {
        i = i.wrapping_sub(1usize);
        bx::push_byte(&mut out, s.as_bytes()[i] as i8);
    }
    return out;
}

fn upper(s: &String) -> String {
    let mut out: String = String::new();
    for i in 0usize..s.len() {
        let c: i8 = s.as_bytes()[i] as i8;
        if (c as i32) >= 97 && (c as i32) <= 122 {
            bx::push_byte(&mut out, ((c as i32) - 32i32) as i8);
        } else {
            bx::push_byte(&mut out, c);
        }
    }
    return out;
}

fn main() {
    let text: String = String::from("  the quick  brown fox jumps ");
    let words: Vec<String> = split(&text);
    print!("{} words\n", words.len());
    for w in words.iter() {
        print!("<{}> {} {}\n", w, reversed(w), upper(w));
    }
    let a: String = String::from("abc");
    let b: String = String::from("abd");
    print!("{} {} {} {}\n", (a < b) as i32, (a == b) as i32, (b >= a) as i32, (a == "abc") as i32);
    let mut c: String = a.clone() + &b + "!" + &(-17).to_string() + &42u64.to_string();
    c.push_str("|");
    c.push_str(&a);
    bx::push_byte(&mut c, 35i8);
    print!("{} {}\n", c, c.len());
    print!("{}|{}|{}|{}\n", bx::substr(&c, 2usize, 3usize), bx::substr(&c, 10usize, 99usize),
           bx::substr_from(&c, 14usize), bx::substr_from(&c, c.len()));
    let e: String = String::new();
    print!("[{}] {} {}\n", e, e.is_empty() as i32, (e < a) as i32);
    let code: i32 = (c.as_bytes()[0usize] as i8) as i32;
    print!("{}\n", code);
}

mod bx {
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

    pub fn substr_from(s: &str, pos: usize) -> String {
        assert!(pos <= s.len(), "bx::substr_from: position past the end (std::out_of_range in C++)");
        String::from(&s[pos..])
    }
}
