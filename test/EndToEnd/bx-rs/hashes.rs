// REQUIRES: cargo
// FR-250: emitrust-bx Rust -> C++, unsigned wraparound end to end (rustc vs
// clang++ stdout byte-diff). Pins: .wrapping_add/.wrapping_sub/.wrapping_mul
// on u32/u64/usize <=> plain C++ unsigned operators (which wrap by
// definition), `x = x.wrapping_mul(k)` <=> `x *= k`, `.wrapping_neg()`,
// `!` on integers <=> `~`, shifts whose amount has a different type, and
// u8 <=> unsigned char. Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn fnv1a32(seed: u32, len: i32) -> u32 {
    let mut h: u32 = 2166136261u32 ^ seed;
    for i in 0..len {
        let b: u8 = (i * 31i32 + 7i32) as u8;
        h ^= b as u32;
        h = h.wrapping_mul(16777619u32);
    }
    return h;
}

fn splitmix64(mut x: u64) -> u64 {
    x = x.wrapping_add(11400714819323198485u64);
    let mut z: u64 = x;
    z = (z ^ (z >> 30i32)).wrapping_mul(13787848793156543929u64);
    z = (z ^ (z >> 27i64)).wrapping_mul(10723151780598845931u64);
    return z ^ (z >> 31u32);
}

fn checksum(n: usize) -> usize {
    let mut acc: usize = 0usize;
    for i in 0usize..n {
        acc = acc.wrapping_mul(31usize).wrapping_add(i ^ (acc >> 7));
    }
    return acc;
}

fn main() {
    print!("{} {} {}\n", fnv1a32(0u32, 10), fnv1a32(3735928559u32, 40), fnv1a32(1u32, 0));
    print!("{} {}\n", splitmix64(0u64), splitmix64(18446744073709551615u64));
    let mut s: u64 = 7u64;
    for r in 0..4 {
        s = splitmix64(s.wrapping_add(r as u64));
        print!("{} ", s % 1000003u64);
    }
    print!("\n");
    print!("{} {}\n", checksum(100usize), checksum(5000usize));
    let small: u32 = 5u32;
    let under: u32 = small.wrapping_sub(9u32);
    print!("{} {} {} {}\n", under, small.wrapping_neg(), !small, under / 3u32);
}
