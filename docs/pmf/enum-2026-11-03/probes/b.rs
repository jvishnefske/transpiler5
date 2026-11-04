// (B) newtype + consts: unscoped enumerators are crate-level consts (they leak in C++ too);
// scoped enumerators are associated consts (`Stage::ERROR`).
#![allow(non_upper_case_globals, non_camel_case_types, non_snake_case)]
mod st {
    #[derive(Clone, Copy, PartialEq, Eq)]
    pub struct Status(pub u32);
    pub const OK: Status = Status(0);
    pub const NONREP: Status = Status(1);
    pub const MEM: Status = Status(2);
    pub const NOT_ENOUGH: Status = Status(7);
    #[derive(Clone, Copy, PartialEq, Eq)]
    pub struct Dup(pub u32);
    pub const A: Dup = Dup(1);
    pub const B: Dup = Dup(1);
    pub const C: Dup = Dup(A.0 + B.0);
    pub const D: Dup = Dup(C.0 + 1);
    #[derive(Clone, Copy, PartialEq, Eq)]
    pub struct Stage(pub i32);
    impl Stage {
        pub const SIGNATURE: Stage = Stage(0);
        pub const HEADER: Stage = Stage(1);
        pub const DONE: Stage = Stage(2);
        pub const ERROR: Stage = Stage(3);
    }
    #[derive(Clone, Copy, PartialEq, Eq)]
    pub struct Mask(pub u32);
    pub const INIT: Mask = Mask(0);
    pub const HUFF_MASK: Mask = Mask(0x10);
    pub const READ_LAST: Mask = Mask(0x11);
    pub const SCAN_MASK: Mask = Mask(0x40);
    pub const READ_SCAN: Mask = Mask(0x41);
    #[derive(Clone, Copy, PartialEq, Eq)]
    pub struct Neg(pub i32);
    pub const M1: Neg = Neg(-1);
    pub const Z: Neg = Neg(0);
    pub const P1: Neg = Neg(1);
    pub fn check(x: i32) -> Status { if x > 2 { return NOT_ENOUGH; } return if x == 0 { OK } else { NONREP }; }
    pub fn fail(s: Status) -> Stage { if s != OK { return Stage::ERROR; } return Stage::DONE; }
    pub fn promote(s: Status) -> i32 { return s.0 as i32 + 1; }
    pub fn is_zero(s: Status) -> i32 { return (s.0 == 0) as i32; }
    pub fn stored(stage: usize) -> usize { return if (stage & HUFF_MASK.0 as usize) != 0 { 1 } else { 0 }; }
    pub fn scoped_to_int(s: Stage) -> i32 { return s.0; }
    pub fn int_to_scoped(v: i32) -> Stage { return Stage(v); }
}
use st::*;
fn main() {
    print!("{} {} {}\n", check(0).0, check(1).0, check(5).0);
    print!("{} {}\n", fail(OK).0, fail(MEM).0);
    print!("{} {}\n", promote(NOT_ENOUGH), is_zero(OK));
    print!("{} {} {} {}\n", A.0, B.0, C.0, D.0);
    print!("{} {}\n", stored(READ_LAST.0 as usize), stored(READ_SCAN.0 as usize));
    print!("{} {}\n", scoped_to_int(Stage::ERROR), scoped_to_int(int_to_scoped(42)));
    print!("{} {} {}\n", M1.0, Z.0, P1.0);
    let mut s: Status = check(3); s = check(0);
    print!("{}\n", (s == OK) as i32);
}
