// REQUIRES: cargo
// FR-260: emitrust-bx Rust -> C++, order-free references end to end (rustc vs
// clang++ byte-diff). Rust items may be used before they are defined; C++
// declare-before-use would reject the same layout, so the C++ printer must
// re-derive a prototype INSIDE its namespace for every forward reference:
// mutual recursion between sibling modules (`evens::even` <=> `odds::odd`),
// a module calling a ROOT function defined after it (`crate::base`), and a
// `&mut Vec` threaded across modules. Distinct values per call site;
// recursion depth is bounded; no overflow.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
mod evens {
    pub fn even(n: i32) -> bool {
        if n == 0 {
            return true;
        }
        return crate::odds::odd(n - 1);
    }
}

mod odds {
    pub fn odd(n: i32) -> bool {
        if n == 0 {
            return false;
        }
        return crate::evens::even(n - 1);
    }
}

mod fill {
    pub fn grow(v: &mut Vec<i64>, n: i32) {
        for i in 0..n {
            v.push(crate::base(i) as i64 * 7i64);
        }
    }

    pub mod sum {
        pub fn all(v: &Vec<i64>) -> i64 {
            let mut s: i64 = 0i64;
            for x in v.iter() {
                s += *x;
            }
            return s;
        }
    }
}

fn base(x: i32) -> i32 {
    return x * x + 3;
}

fn main() {
    print!("{} {} {} {}\n", evens::even(12) as i32, evens::even(9) as i32, odds::odd(15) as i32, odds::odd(2) as i32);
    let mut v: Vec<i64> = Vec::new();
    fill::grow(&mut v, 6);
    crate::fill::grow(&mut v, 2);
    print!("{} {}\n", v.len(), fill::sum::all(&v));
}
