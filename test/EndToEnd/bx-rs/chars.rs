// REQUIRES: cargo
// FR-250: emitrust-bx Rust -> C++, the char rows end to end (rustc vs
// clang++ byte-diff). C++ `char` is SIGNED on x86-64 and maps to i8 (never
// u8); `unsigned char` maps to u8. Pins: i8/u8 literals (`65i8` <=> 'A'),
// i32 -> i8 narrowing that wraps (200 -> -56), sign-extending i8 -> i32,
// zero-extending u8 -> i32, and bx::chr(x) <=> %c on ASCII (the helper
// panics, loudly, on a byte >= 0x80 rather than emit different bytes).
// Integer literals inside an `as` operand carry a suffix: rustc types an
// unsuffixed one from the cast TARGET (`200 as i8` is a hard error), and
// the subset refuses to guess.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn caesar(c: i8, shift: i32) -> i8 {
    let base: i32 = 97;
    let off: i32 = ((c as i32) - base + shift) % 26;
    return (base + off) as i8;
}

fn upper(c: i8) -> i8 {
    if (c as i32) >= 97 && (c as i32) <= 122 {
        return ((c as i32) - 32i32) as i8;
    }
    return c;
}

fn main() {
    let word_start: i8 = 104i8;
    for k in 0..5 {
        let c: i8 = caesar(((word_start as i32) + k * 3i32) as i8, 13);
        print!("{}", bx::chr(upper(c) as i32));
    }
    print!("\n");
    let wrapped: i8 = 200i32 as i8;
    let back: u8 = wrapped as u8;
    print!("{} {} {} {}\n", wrapped as i32, back as i32, (back as i8) as i32, ((-1i32) as u8) as i32);
    let a: i8 = 65i8;
    let z: i8 = 122i8;
    print!("[{}{}{}] {} {}\n", bx::chr(a as i32), bx::chr(z as i32), bx::chr(48 + 7), (z as i32) - (a as i32), caesar(z, 1) as i32);
}

mod bx {
    pub fn chr(x: i32) -> char {
        let b: u8 = x as u8;
        assert!(b < 128, "bx::chr: printf %c of a non-ASCII byte is outside the subset");
        b as char
    }
}
