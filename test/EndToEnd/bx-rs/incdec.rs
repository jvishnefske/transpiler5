// REQUIRES: cargo
// FR-259: emitrust-bx Rust -> C++, the canonical increment BLOCKS end to
// end (rustc vs clang++ byte-diff). `{ x += 1; x }` <=> `++x`, `{ x += 1; x -
// 1 }` <=> `x++`, `{ x -= 1; x }` / `{ x -= 1; x + 1 }` <=> `--x` / `x--`, the
// unsigned spellings `{ p = p.wrapping_add(1usize); p.wrapping_sub(1usize) }`
// and a reference `{ *n = (*n).wrapping_add(1u32); (*n).wrapping_sub(1u32) }`:
// a byte writer `out[pos++] = ..`, a reader through a `&mut usize` cursor, a
// `while ({ k -= 1; k + 1 }) > 0` countdown, a usize decremented through 0
// (the wrap is the observed value), an i64 counter and an increment as a call
// argument. Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn write2(out: &mut Vec<u8>, mut pos: usize, a: u8, b: u8) -> usize {
    out[{ pos = pos.wrapping_add(1usize); pos.wrapping_sub(1usize) }] = a;
    out[{ pos = pos.wrapping_add(1usize); pos.wrapping_sub(1usize) }] = b;
    return pos;
}

fn read(src: &Vec<u8>, cur: &mut usize) -> u32 {
    return src[{ *cur = (*cur).wrapping_add(1usize); (*cur).wrapping_sub(1usize) }] as u32;
}

fn countdown(mut k: i32) -> i32 {
    let mut s: i32 = 0;
    while ({ k -= 1; k + 1 }) > 0 {
        s = (s * 3) + k;
    }
    return s + k;
}

fn wrap(mut z: usize) -> usize {
    let before: usize = { z = z.wrapping_sub(1usize); z.wrapping_add(1usize) };
    let after: usize = { z = z.wrapping_sub(1usize); z };
    return before.wrapping_add(after % 1000usize);
}

fn twice(x: i64) -> i64 {
    return x * 2i64;
}

fn counters(mut t: i64, n: &mut u32) -> i64 {
    let a: i64 = twice({ t += 1i64; t - 1i64 });
    let b: i64 = { t += 1i64; t };
    let c: u32 = { *n = (*n).wrapping_add(1u32); (*n).wrapping_sub(1u32) };
    let d: u32 = { *n = (*n).wrapping_sub(1u32); *n };
    return (a * 1000i64) + (b * 10i64) + (c as i64) + ((d % 100u32) as i64);
}

fn main() {
    let mut buf: Vec<u8> = vec![0u8; 4usize];
    let p: usize = write2(&mut buf, 0usize, 18u8, 52u8);
    let q: usize = write2(&mut buf, p, 250u8, 7u8);
    let mut cur: usize = 1usize;
    let x: u32 = read(&buf, &mut cur);
    let y: u32 = read(&buf, &mut cur);
    print!("{} {} {} {} {}\n", q, x, y, cur, buf[3usize] as i32);
    print!("{} {}\n", countdown(5), countdown(0));
    print!("{} {}\n", wrap(0usize) % 100000usize, wrap(9usize));
    let mut m: u32 = 0u32;
    let mut k: u32 = 41u32;
    print!("{} {}\n", counters(7i64, &mut m), counters(-12i64, &mut k));
    print!("{} {}\n", m, k);
}
