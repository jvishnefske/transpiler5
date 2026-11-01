// FR-259: zero `unsafe` in anything emitrust-bx emits for the cheap-breadth
// rows. `?:` is a safe `if` expression, an increment used as a value a plain
// block (`{ x += 1; x - 1 }`, never `std::ptr::read` / a raw-pointer swap),
// `std::uint8_t` / `std::int16_t` builtin integer types, `static` / `inline`
// a plain private `fn`, `auto` an explicitly typed `let` -- and no helper in
// `mod bx` is needed for any of them (the rubric scores unsafe; a helper that
// hid it would relocate the obligation, not discharge it).
// RUN: %emitrust-bx --emit=crate %s -o %t.crate
// RUN: grep -c unsafe %t.crate/src/main.rs > %t.count || true
// RUN: FileCheck --check-prefix=COUNT --input-file=%t.count %s
// RUN: FileCheck --check-prefix=SHAPE --input-file=%t.crate/src/main.rs %s
// RUN: not grep -q "mod bx" %t.crate/src/main.rs
// COUNT: {{^}}0{{$}}
// SHAPE: fn next(v: &Vec<u8>, pos: &mut usize) -> u8 {
// SHAPE: return v[{ *pos = (*pos).wrapping_add(1usize); (*pos).wrapping_sub(1usize) }];
// SHAPE: fn sign(mut x: i16) -> i32 {
// SHAPE: return if (x as i32) < 0 { -1 } else if (x as i32) > 0 { 1 } else { 0 };
// SHAPE: let mut p: usize = 0usize;
// SHAPE: let mut a: u8 = next(&v, &mut p);
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

static std::uint8_t next(const std::vector<std::uint8_t>& v, std::size_t& pos) {
    return v[pos++];
}

inline int sign(std::int16_t x) {
    return x < 0 ? -1 : x > 0 ? 1 : 0;
}

int main() {
    std::vector<std::uint8_t> v = {5, 9};
    auto p = static_cast<std::size_t>(0);
    auto a = next(v, p);
    std::printf("%d %d %d\n", a, next(v, p), sign(-4));
    return 0;
}
