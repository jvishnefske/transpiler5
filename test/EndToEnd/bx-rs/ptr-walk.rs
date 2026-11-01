// REQUIRES: cargo
// FR-261: emitrust-bx Rust -> C++, re-slicing end to end (rustc vs clang++
// byte-diff). `p = &p[1usize..];` on a `&[i64]` and `out = &mut
// out[k..];` on a `&mut [i64]` parameter (both borrow-check on rustc 1.96 and
// 1.98) <=> `++p;` / `out += k;`; slice LOCALS `let mut q: &[i64] = &xs[2usize..];`
// <=> `const long long* q = xs + 2;` and `let w: &mut [i64] = &mut v[1usize..];`
// <=> `long long* const w = v.data() + 1;` (a non-`mut` binding is a const
// pointer), a mutable local that ends before its vector is used again, and
// passing a slice variable on unchanged (`prefix_sum(q, ..)`). Distinct values
// at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn prefix_sum(mut p: &[i64], mut n: usize) -> i64 {
    let mut s: i64 = 0i64;
    while n > 0usize {
        s = (s * 10i64) + p[0usize];
        p = &p[1usize..];
        n = n.wrapping_sub(1usize);
    }
    return s;
}

fn spread(mut out: &mut [i64], n: usize, step: usize, v: i64) {
    for i in 0usize..n {
        out[0usize] = v + (i as i64);
        if i.wrapping_add(1usize) < n {
            out = &mut out[step..];
        }
    }
}

fn tail_sum(xs: &[i64], from: usize, n: usize) -> i64 {
    let mut q: &[i64] = &xs[from..];
    let mut t: i64 = 0i64;
    for i in 0usize..n {
        t += q[i];
    }
    q = &q[n..];
    return t + prefix_sum(q, 1usize);
}

fn main() {
    let mut v: Vec<i64> = vec![0i64; 10usize];
    {
        let w: &mut [i64] = &mut v[1usize..];
        spread(w, 4usize, 2usize, 3i64);
    }
    spread(&mut v[2usize..], 3usize, 3usize, 20i64);
    print!("{} {} {}\n", prefix_sum(&v[..], 5usize), prefix_sum(&v[5usize..], 3usize), tail_sum(&v[..], 2usize, 3usize));
    for i in 0usize..v.len() {
        print!("{} ", v[i]);
    }
    print!("\n");
}
