// FR-261: Rust slices outside the canonical subset are LOCATED errors, ALL of
// them (not the first). Pins: the `&` / `&mut` of a re-slice and the
// `from_ref` / `from_mut` of a one-element slice must be the ones the
// PARAMETER fixes (`&mut v[..]` for a `&[i32]` is accepted by rustc but is not
// the canonical spelling; `&v[..]` / `from_ref` for a `&mut [i32]` is a rustc
// error the subset names first); `&s[..]` of a slice variable is spelled `s`;
// whole-slice assignment `w = s;` (only `w = &w[k..];` is a re-slice); slice
// `.len()` and slice `==` (no C++ pointer equivalent; `==` would compare
// contents); and R2 -- `v` used while the `&mut` slice `q` of it is in scope.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}11{{$}}
fn sum(p: &[i32], n: usize) -> i32 {
    let mut t: i32 = 0;
    for i in 0usize..n {
        t += p[i];
    }
    return t;
}

fn set(p: &mut [i32], v: i32) {
    p[0usize] = v;
}

fn main() {
    let mut v: Vec<i32> = vec![1, 2, 3];
    let mut x: i32 = 4;
    // CHECK: pointer-rust.rs:[[@LINE+1]]:27: error: unsupported: argument 1 of 'sum' is not spelled the canonical way (`&x[..]`)
    let a: i32 = sum(&mut v[..], 2usize);
    // CHECK-NEXT: pointer-rust.rs:[[@LINE+1]]:9: error: unsupported: argument 1 of 'set' is not spelled the canonical way (`std::slice::from_mut(&mut x)`)
    set(std::slice::from_ref(&x), 5);
    // CHECK-NEXT: pointer-rust.rs:[[@LINE+1]]:10: error: unsupported: argument 1 of 'set' is not spelled the canonical way (`&mut x[..]`)
    set(&v[1usize..], 6);
    let s: &[i32] = &v[..];
    // CHECK-NEXT: pointer-rust.rs:[[@LINE+1]]:22: error: unsupported: whole slice `&p[..]` of a pointer (the subset spells it `p`)
    let t: &[i32] = &s[..];
    let b: i32 = sum(&v[..], v.len()) + sum(s, 1usize) + sum(t, 1usize);
    // CHECK-NEXT: pointer-rust.rs:[[@LINE+1]]:22: error: size of a value of type &[i32]
    let n: usize = s.len();
    let mut w: &[i32] = &v[1usize..];
    // CHECK-NEXT: pointer-rust.rs:[[@LINE+1]]:5: error: unsupported: assignment of a slice other than a re-slice of itself (`w = &w[k..];`)
    w = s;
    // CHECK-NEXT: pointer-rust.rs:[[@LINE+1]]:24: error: comparison of &[i32]
    let same: bool = s == t;
    // CHECK-NEXT: pointer-rust.rs:[[@LINE+3]]:5: error: unsupported: 'v' is modified or moved while 's', a pointer into it, is in scope (a borrow conflict in Rust)
    // CHECK-NEXT: pointer-rust.rs:[[@LINE+2]]:5: error: unsupported: 'v' is modified or moved while 't', a pointer into it, is in scope (a borrow conflict in Rust)
    // CHECK-NEXT: pointer-rust.rs:[[@LINE+1]]:5: error: unsupported: 'v' is modified or moved while 'w', a pointer into it, is in scope (a borrow conflict in Rust)
    let q: &mut [i32] = &mut v[..];
    // CHECK-NEXT: pointer-rust.rs:[[@LINE+1]]:5: error: unsupported: 'v' is used while 'q', a mutable pointer derived from it, is in scope (a second borrow in Rust)
    v[0usize] = 7;
    q[0usize] = 8;
    print!("{} {} {} {}\n", a, b, n, same as i32);
}
