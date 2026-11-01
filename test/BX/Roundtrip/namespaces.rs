// FR-260: Rust modules -> C++ namespaces, law 2 at the CLI from the Rust
// side (the C++ printed for this file, fed back in, prints byte-identical
// C++; its Rust is this file's canonical Rust). Pins that rs_parse resolves
// `self::`, `super::`, `super::super::` and a relative child path
// (`text::show` from the root) with Rust's rules to the same absolute
// Kernel path the canonical `crate::` spelling names, that a private `fn`
// inside a module is a C++ anonymous-namespace member reachable from a
// nested module (`crate::text::first` <=> `::text::first`), and that the
// safe helpers are `crate::bx::..` inside a module (plain `bx::` would
// resolve relative to it). Every module's `tag` is a different function.
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: %emitrust-bx --emit=cpp %t.cpp -o %t.2.cpp
// RUN: diff %t.cpp %t.2.cpp
// RUN: %emitrust-bx --emit=rust %t.cpp -o %t.rs
// RUN: %emitrust-bx --emit=rust %s -o %t.self.rs
// RUN: diff %t.rs %t.self.rs
// RUN: %emitrust-bx --check-roundtrip --emit=rust %s | FileCheck %s
// RUN: FileCheck --check-prefix=CPP %s --input-file=%t.cpp
fn tag() -> i32 {
    return 1;
}

mod text {
    fn first(s: &String) -> String {
        return crate::bx::substr(s, 0usize, 2usize);
    }

    pub fn tag() -> i32 {
        return 20;
    }

    pub fn show(s: &String) {
        print!("{} {} {}\n", first(s), crate::bx::fmt_f(2.5, 2), tag() + super::tag());
    }

    pub mod deep {
        pub fn tag() -> i32 {
            return 300;
        }

        pub fn sum() -> i32 {
            return self::tag() + super::tag() + super::super::tag() + crate::text::first(&String::from("ab")).len() as i32;
        }
    }
}

mod math {
    pub fn tag() -> i32 {
        return 4000;
    }
}

fn main() {
    text::show(&String::from("hello"));
    print!("{} {}\n", text::deep::sum(), crate::math::tag() + tag());
}

mod bx {
    pub fn fmt_f(x: f64, prec: usize) -> String {
        if x.is_nan() {
            if x.is_sign_negative() {
                return String::from("-nan");
            }
            return String::from("nan");
        }
        format!("{:.*}", prec, x)
    }

    pub fn substr(s: &str, pos: usize, n: usize) -> String {
        assert!(pos <= s.len(), "bx::substr: position past the end (std::out_of_range in C++)");
        let end: usize = pos + std::cmp::min(n, s.len() - pos);
        String::from(&s[pos..end])
    }
}

// CHECK: fn tag() -> i32 {
// CHECK-NEXT:     return 1;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: mod text {
// CHECK-NEXT:     fn first(s: &String) -> String {
// CHECK-NEXT:         return crate::bx::substr(s, 0usize, 2usize);
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn tag() -> i32 {
// CHECK-NEXT:         return 20;
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn show(s: &String) {
// CHECK-NEXT:         print!("{} {} {}\n", first(s), crate::bx::fmt_f(2.5, 2), tag() + crate::tag());
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub mod deep {
// CHECK-NEXT:         pub fn tag() -> i32 {
// CHECK-NEXT:             return 300;
// CHECK-NEXT:         }
// CHECK-EMPTY:
// CHECK-NEXT:         pub fn sum() -> i32 {
// CHECK-NEXT:             return tag() + crate::text::tag() + crate::tag() + (crate::text::first(&String::from("ab")).len() as i32);
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: mod math {
// CHECK-NEXT:     pub fn tag() -> i32 {
// CHECK-NEXT:         return 4000;
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     crate::text::show(&String::from("hello"));
// CHECK-NEXT:     print!("{} {}\n", crate::text::deep::sum(), crate::math::tag() + tag());
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: mod bx {
// CHECK-NEXT:     pub fn fmt_f(x: f64, prec: usize) -> String {
// CHECK-NEXT:         if x.is_nan() {
// CHECK-NEXT:             if x.is_sign_negative() {
// CHECK-NEXT:                 return String::from("-nan");
// CHECK-NEXT:             }
// CHECK-NEXT:             return String::from("nan");
// CHECK-NEXT:         }
// CHECK-NEXT:         format!("{:.*}", prec, x)
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn substr(s: &str, pos: usize, n: usize) -> String {
// CHECK-NEXT:         assert!(pos <= s.len(), "bx::substr: position past the end (std::out_of_range in C++)");
// CHECK-NEXT:         let end: usize = pos + std::cmp::min(n, s.len() - pos);
// CHECK-NEXT:         String::from(&s[pos..end])
// CHECK-NEXT:     }
// CHECK-NEXT: }

// CPP: #include <cstdio>
// CPP-NEXT: #include <string>
// CPP-EMPTY:
// CPP-NEXT: int tag() {
// CPP-NEXT:     return 1;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: namespace text {
// CPP-EMPTY:
// CPP-NEXT: namespace {
// CPP-EMPTY:
// CPP-NEXT: std::string first(const std::string& s) {
// CPP-NEXT:     return s.substr(static_cast<std::size_t>(0), static_cast<std::size_t>(2));
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace
// CPP-EMPTY:
// CPP-NEXT: int tag() {
// CPP-NEXT:     return 20;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void show(const std::string& s) {
// CPP-NEXT:     std::printf("%s %.2f %d\n", first(s).c_str(), 2.5, tag() + ::tag());
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: namespace deep {
// CPP-EMPTY:
// CPP-NEXT: int tag() {
// CPP-NEXT:     return 300;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int sum() {
// CPP-NEXT:     return tag() + ::text::tag() + ::tag() + static_cast<int>(::text::first(std::string("ab")).size());
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace deep
// CPP-EMPTY:
// CPP-NEXT: }  // namespace text
// CPP-EMPTY:
// CPP-NEXT: namespace math {
// CPP-EMPTY:
// CPP-NEXT: int tag() {
// CPP-NEXT:     return 4000;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace math
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     ::text::show(std::string("hello"));
// CPP-NEXT:     std::printf("%d %d\n", ::text::deep::sum(), ::math::tag() + tag());
// CPP-NEXT:     return 0;
// CPP-NEXT: }
