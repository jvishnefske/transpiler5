// FR-259: Rust if-EXPRESSIONS and block expressions outside the canonical
// subset are LOCATED errors, each reported (parse errors stop at the first,
// so each is its own `split` unit below). The canonical increment blocks are
// exactly `{ x += 1; x }` / `{ x += 1; x - 1 }` / `{ x -= 1; x }` / `{ x -=
// 1; x + 1 }` (unsigned: `.wrapping_add(1T)` / `.wrapping_sub(1T)`; a
// reference `*x`); anything else in braces is refused, as is an `if` /
// block expression followed -- unparenthesized -- by a method call or a
// binary operator (statement-position rules make those fragile in rustc;
// the printer parenthesizes), an `if` expression without `else` or with a
// statement in an arm, arms of different types, i16 arithmetic (C++ promotes
// it; the subset spells the casts), and an increment beside another use of
// its variable.
// RUN: split-file %s %t
// RUN: not %emitrust-bx %t/block-step.rs -o %t/block-step.out 2>&1 | FileCheck --check-prefix=BLOCK-STEP %s
// RUN: not %emitrust-bx %t/block-tail.rs -o %t/block-tail.out 2>&1 | FileCheck --check-prefix=BLOCK-TAIL %s
// RUN: not %emitrust-bx %t/block-stmts.rs -o %t/block-stmts.out 2>&1 | FileCheck --check-prefix=BLOCK-STMTS %s
// RUN: not %emitrust-bx %t/dot-after-if.rs -o %t/dot-after-if.out 2>&1 | FileCheck --check-prefix=DOT-AFTER-IF %s
// RUN: not %emitrust-bx %t/plus-after-if.rs -o %t/plus-after-if.out 2>&1 | FileCheck --check-prefix=PLUS-AFTER-IF %s
// RUN: not %emitrust-bx %t/star-after-block.rs -o %t/star-after-block.out 2>&1 | FileCheck --check-prefix=STAR-AFTER-BLOCK %s
// RUN: not %emitrust-bx %t/if-no-else.rs -o %t/if-no-else.out 2>&1 | FileCheck --check-prefix=IF-NO-ELSE %s
// RUN: not %emitrust-bx %t/if-arm-stmt.rs -o %t/if-arm-stmt.out 2>&1 | FileCheck --check-prefix=IF-ARM-STMT %s
// RUN: not %emitrust-bx %t/wf.rs -o %t/wf.out 2> %t/wf.err
// RUN: FileCheck --check-prefix=WF %s --input-file=%t/wf.err
// RUN: grep -c "error:" %t/wf.err | FileCheck --check-prefix=WF-COUNT %s
// RUN: not test -e %t/wf.out
// BLOCK-STEP: block-step.rs:2:18: error: unsupported: block expression other than the canonical increment `{ x += 1; x }` / `{ x += 1; x - 1 }` (`{ x = x.wrapping_add(1u32); x }` when unsigned)
// BLOCK-TAIL: block-tail.rs:2:18: error: unsupported: block expression other than the canonical increment `{ x += 1; x }` / `{ x += 1; x - 1 }` (`{ x = x.wrapping_add(1u32); x }` when unsigned)
// BLOCK-STMTS: block-stmts.rs:2:18: error: unsupported: block expression other than the canonical increment `{ x += 1; x }` / `{ x += 1; x - 1 }` (`{ x = x.wrapping_add(1u32); x }` when unsigned)
// DOT-AFTER-IF: dot-after-if.rs:2:39: error: unsupported: `.` after an unparenthesized `if` expression (the subset parenthesizes it)
// PLUS-AFTER-IF: plus-after-if.rs:2:40: error: unsupported: `+` after an unparenthesized `if` expression (the subset parenthesizes it)
// STAR-AFTER-BLOCK: star-after-block.rs:2:32: error: unsupported: `*` after an unparenthesized block expression (the subset parenthesizes it)
// IF-NO-ELSE: if-no-else.rs:2:18: error: unsupported: `if` expression without `else`
// IF-ARM-STMT: if-arm-stmt.rs:2:25: error: unsupported: `let` expression
// WF:      wf.rs:2:20: error: unsupported: increment block not spelled the canonical way (`{ x = x.wrapping_add(1usize); .. }`)
// WF-NEXT: wf.rs:3:18: error: unsupported: increment block of 'r' not spelled the canonical way (`*r`)
// WF-NEXT: wf.rs:4:18: error: arms of a conditional expression have different types i32 and i64
// WF-NEXT: wf.rs:5:20: error: arithmetic add on i16
// WF-NEXT: wf.rs:6:19: error: unsupported: increment/decrement of 'n' used as a value in a statement that also uses 'n' (C++ leaves the order unsequenced or unspecified)
// WF-COUNT: {{^}}5{{$}}
//--- block-step.rs
fn f(mut n: i32, c: bool) -> i32 {
    let a: i32 = { n += 2; n };
    return a;
}
//--- block-tail.rs
fn f(mut n: i32, c: bool) -> i32 {
    let a: i32 = { n += 1; n - 2 };
    return a;
}
//--- block-stmts.rs
fn f(mut n: i32, c: bool) -> i32 {
    let a: i32 = { n += 1; n += 1; n };
    return a;
}
//--- dot-after-if.rs
fn f(mut n: i32, c: bool) -> i32 {
    let a: i32 = if c { 1 } else { 2 }.max(3);
    return a;
}
//--- plus-after-if.rs
fn f(mut n: i32, c: bool) -> i32 {
    let a: i32 = if c { 1 } else { 2 } + 3;
    return a;
}
//--- star-after-block.rs
fn f(mut n: i32, c: bool) -> i32 {
    let a: i32 = { n += 1; n } * 2;
    return a;
}
//--- if-no-else.rs
fn f(mut n: i32, c: bool) -> i32 {
    let a: i32 = if c { 1 };
    return a;
}
//--- if-arm-stmt.rs
fn f(mut n: i32, c: bool) -> i32 {
    let a: i32 = if c { let q: i32 = 1; q } else { 2 };
    return a;
}
//--- wf.rs
fn f(mut n: i32, mut z: usize, c: bool, r: &mut i32, q: i16) -> i32 {
    let d: usize = { z += 1usize; z };
    let e: i32 = { r += 1; r };
    let p: i32 = if c { 1 } else { 2i64 };
    let s: i16 = q + q;
    let t: i32 = ({ n += 1; n }) + n;
    return (d as i32) + e + p + (s as i32) + t;
}
