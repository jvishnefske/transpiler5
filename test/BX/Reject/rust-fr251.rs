// FR-251: the Rust side of the vector/string/reference subset is DEFINED by
// rs_parse + the surface check, and every well-formedness violation is
// reported (located, all of them -- only a SYNTAX error stops at the
// first). Pins: a reference argument spelled without `&` / with the wrong
// `&` / `&mut` (the subset reads the borrow kind off the parameter type and
// requires the canonical spelling), `*` of a non-reference, `f(&mut a, &a)`
// aliasing, a move of an immutable binding (its C++ mirror, a const object,
// cannot be moved -- it would copy), a use after a move, a push inside
// `for x in a.iter()`, and an element write whose index reads the same
// vector. rustc itself rejects several of these; the point is that the
// subset says so first, in its own located terms, instead of emitting C++.
// RUN: not %emitrust-bx %s -o %t.cpp 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.cpp
// COUNT: {{^}}8{{$}}
fn total(v: &Vec<i32>) -> i32 {
    let mut s: i32 = 0;
    for x in v.iter() {
        s += *x;
    }
    return s;
}

fn grow(v: &mut Vec<i32>, by: &Vec<i32>) {
    v.push(by.len() as i32);
}

fn main() {
    let mut a: Vec<i32> = vec![1, 2];
    // CHECK: rust-fr251.rs:[[@LINE+1]]:24: error: unsupported: argument 1 of 'total' binds a reference; the subset spells it `&x`
    let n: i32 = total(a);
    // CHECK-NEXT: rust-fr251.rs:[[@LINE+1]]:24: error: unsupported: argument 1 of 'total' is not spelled the canonical way (`&x`)
    let t: i32 = total(&mut a);
    let k: i32 = 3;
    // CHECK-NEXT: rust-fr251.rs:[[@LINE+1]]:19: error: unsupported: `*` of 'k', which is not a reference
    let j: i32 = *k;
    // CHECK-NEXT: rust-fr251.rs:[[@LINE+1]]:5: error: unsupported: 'a' is passed to 'grow' by non-const reference and also used by another argument of the same call (aliasing)
    grow(&mut a, &a);
    let b: Vec<i32> = vec![4];
    // CHECK-NEXT: rust-fr251.rs:[[@LINE+1]]:23: error: unsupported: move of immutable 'b' (C++ cannot move a const object; it would copy)
    let c: Vec<i32> = b;
    let mut d: Vec<i32> = vec![5];
    // CHECK-NEXT: rust-fr251.rs:[[@LINE+1]]:23: error: unsupported: 'd' is used after it is moved (std::move(d) <=> d requires no later mention of d)
    let e: Vec<i32> = d;
    d.push(6);
    // CHECK-NEXT: rust-fr251.rs:[[@LINE+1]]:5: error: unsupported: the body of a range-for over 'a' modifies 'a' (iterator invalidation in C++, a borrow conflict in Rust)
    for x in a.iter() {
        a.push(*x);
    }
    // CHECK-NEXT: rust-fr251.rs:[[@LINE+1]]:5: error: unsupported: an element of 'a' is written through an index that reads 'a' (a second borrow in Rust)
    a[a.len().wrapping_sub(1usize)] = 9;
    print!("{} {} {} {} {}\n", n, t, j, c.len(), e.len());
}
