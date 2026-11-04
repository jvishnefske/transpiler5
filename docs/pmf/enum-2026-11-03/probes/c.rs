// (C) erasure: enum type -> its integer type, enumerators -> consts (FR-264 ConstItem)
#![allow(non_upper_case_globals)]
mod st {
    pub const OK: i32 = 0; pub const NONREP: i32 = 1; pub const MEM: i32 = 2; pub const NOT_ENOUGH: i32 = 7;
    pub const A: i32 = 1; pub const B: i32 = 1; pub const C: i32 = A + B; pub const D: i32 = C + 1;
    pub mod Stage { pub const SIGNATURE: i32 = 0; pub const HEADER: i32 = 1; pub const DONE: i32 = 2; pub const ERROR: i32 = 3; }
    pub const INIT: i32 = 0; pub const HUFF_MASK: i32 = 0x10; pub const READ_LAST: i32 = 0x11; pub const SCAN_MASK: i32 = 0x40; pub const READ_SCAN: i32 = 0x41;
    pub const M1: i32 = -1; pub const Z: i32 = 0; pub const P1: i32 = 1;
    pub fn check(x: i32) -> i32 { if x > 2 { return NOT_ENOUGH; } return if x == 0 { OK } else { NONREP }; }
    pub fn fail(s: i32) -> i32 { if s != OK { return Stage::ERROR; } return Stage::DONE; }
    pub fn promote(s: i32) -> i32 { return s + 1; }
    pub fn is_zero(s: i32) -> i32 { return (s == 0) as i32; }
    pub fn stored(stage: usize) -> usize { return if (stage & HUFF_MASK as usize) != 0 { 1 } else { 0 }; }
    pub fn scoped_to_int(s: i32) -> i32 { return s; }
    pub fn int_to_scoped(v: i32) -> i32 { return v; }
}
use st::*;
fn main() {
    print!("{} {} {}\n", check(0), check(1), check(5));
    print!("{} {}\n", fail(OK), fail(MEM));
    print!("{} {}\n", promote(NOT_ENOUGH), is_zero(OK));
    print!("{} {} {} {}\n", A, B, C, D);
    print!("{} {}\n", stored(READ_LAST as usize), stored(READ_SCAN as usize));
    print!("{} {}\n", scoped_to_int(Stage::ERROR), scoped_to_int(int_to_scoped(42)));
    print!("{} {} {}\n", M1, Z, P1);
    let mut s: i32 = check(3); s = check(0);
    print!("{}\n", (s == OK) as i32);
}
