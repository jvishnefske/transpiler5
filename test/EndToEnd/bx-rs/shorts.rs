// REQUIRES: cargo
// FR-259: emitrust-bx Rust -> C++, the 16-bit integer types end to end
// (rustc vs clang++ byte-diff). `i16` <=> `short`, `u16` <=> `unsigned
// short`, with NO arithmetic of their own: every operand is cast to i32 first
// (C++'s integer promotion, explicit), so `((a as i32) + (b as i32)) as u16`
// wraps only at the narrowing cast in both languages; literals `5i16` <=>
// `static_cast<short>(5)`; `Vec<u16>` elements, parameters, returns, a
// struct field, `as` between i16 / u16 / i8 / i64 / u32 (modular), and an
// i16 counting accumulator. Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
#[derive(Clone)]
struct Sample {
    pub left: i16,
    pub right: u16,
}

fn add_wrap(a: u16, b: u16) -> u16 {
    return ((a as i32) + (b as i32)) as u16;
}

fn mid(s: &Sample) -> i32 {
    return ((s.left as i32) + (s.right as i32)) / 2;
}

fn checksum(v: &Vec<u16>) -> u32 {
    let mut h: u32 = 7u32;
    for x in v.iter() {
        h = h.wrapping_mul(31u32).wrapping_add((*x) as u32);
    }
    return h;
}

fn narrow(x: i64) -> i16 {
    return x as i16;
}

fn main() {
    let s: Sample = Sample { left: (-300i32) as i16, right: 65000u16 };
    let mut v: Vec<u16> = vec![1u16, 65535u16, 4096u16];
    v.push(add_wrap(65530u16, 17u16));
    let mut acc: i16 = 0i16;
    for i in 0usize..v.len() {
        acc = ((acc as i32) + ((v[i] as i32) % 1000i32)) as i16;
    }
    print!("{} {} {}\n", add_wrap(65535u16, 2u16) as i32, add_wrap(100u16, 23u16) as i32, mid(&s));
    print!("{} {}\n", checksum(&v), acc as i32);
    print!("{} {} {}\n", narrow(-40000i64) as i32, narrow(70001i64) as i32, (((s.right as i32) as i16) as i32));
    print!("{} {}\n", ((s.left as u16) as i32), ((v[1usize] as i8) as i32));
}
