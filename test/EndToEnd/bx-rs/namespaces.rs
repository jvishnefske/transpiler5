// REQUIRES: cargo
// FR-260: emitrust-bx Rust -> C++, modules end to end (rustc vs clang++
// byte-diff). Pins `mod` <=> `namespace`, nested `pub mod`, and every call
// spelling rs_parse resolves with Rust's rules: bare `scale` (the CURRENT
// module only -- C++ then finds the same, innermost, function), `self::`,
// `super::`, `super::super::`, `crate::` paths, a relative child path from
// the root, and sibling modules defining the same name. The C++ printer
// spells them `scale` / `::geo::scale`. Distinct values per call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn scale(x: i32) -> i32 {
    return x + 1;
}

mod geo {
    pub fn scale(x: i32) -> i32 {
        return x * 10;
    }

    pub fn area(w: i32, h: i32) -> i32 {
        return scale(w) * h + super::scale(h);
    }

    pub mod solid {
        pub fn scale(x: i32) -> i32 {
            return x * 100;
        }

        pub fn volume(w: i32, h: i32, d: i32) -> i64 {
            return (self::scale(w) + super::scale(h) + super::super::scale(d)) as i64 * 3i64;
        }

        pub mod deep {
            pub fn cube(x: i64) -> i64 {
                return x * x * x + crate::geo::solid::scale(x as i32) as i64;
            }
        }
    }
}

mod units {
    pub fn area(w: i32, h: i32) -> i32 {
        return w * h - 3;
    }

    pub fn total(v: &Vec<i32>) -> i32 {
        let mut s: i32 = 0;
        for x in v.iter() {
            s += crate::geo::area(*x, 2) - area(*x, 2);
        }
        return s;
    }
}

fn main() {
    print!("{} {} {}\n", scale(4), geo::scale(5), crate::geo::solid::scale(6));
    print!("{} {}\n", crate::geo::area(7, 3), units::area(7, 3));
    print!("{} {}\n", geo::solid::volume(2, 3, 4), crate::geo::solid::volume(9, 8, 7));
    print!("{} {}\n", crate::geo::solid::deep::cube(11i64), geo::solid::deep::cube(-12i64));
    let v: Vec<i32> = vec![1, 5, 9];
    print!("{}\n", crate::units::total(&v));
}
