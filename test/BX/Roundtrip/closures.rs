// FR-252: Rust -> C++, law 2 at the CLI from the Rust side (the C++ printed
// for this file, fed back in, prints byte-identical C++; its Rust is this
// file's canonical Rust, byte for byte). Pins that rs_parse reads `let mut f
// = move |x: i32| -> i32 {..};` as a `mutable` lambda over its own copy
// `[seen]`, a non-move closure as by-reference captures `[&log]`, `||` as an
// empty parameter list, a call `f(x)` of a closure (a local shadows a
// function of its name) or of a callable parameter as the closure's
// `operator()`, `&impl Fn(..) -> R` / `&mut impl FnMut(..) -> R` as `const
// F&` / `F&` of `template <typename F>` (in a module too), a callable
// parameter passed on bare (`drive(f, 2)`), and `&mut acc` / `&less` at a call
// site as the closure itself.
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: %emitrust-bx --emit=cpp %t.cpp -o %t.2.cpp
// RUN: diff %t.cpp %t.2.cpp
// RUN: %emitrust-bx --emit=rust %t.cpp -o %t.rs
// RUN: %emitrust-bx --emit=rust %s -o %t.self.rs
// RUN: diff %t.rs %t.self.rs
// RUN: %emitrust-bx --check-roundtrip --emit=rust %s | FileCheck %s
// RUN: FileCheck --check-prefix=CPP %s --input-file=%t.cpp
// CHECK:      mod calc {
// CHECK-NEXT:     pub fn pick(f: &impl Fn(i32, i32) -> bool, a: i32, b: i32) -> i32 {
// CHECK-NEXT:         if f(a, b) {
// CHECK-NEXT:             return a;
// CHECK-NEXT:         }
// CHECK-NEXT:         return b;
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn drive(f: &mut impl FnMut(i32) -> i32, times: i32) -> i32 {
// CHECK-NEXT:         let mut last: i32 = 0;
// CHECK-NEXT:         for i in 0..times {
// CHECK-NEXT:             last = f(i);
// CHECK-NEXT:         }
// CHECK-NEXT:         return last;
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn again(f: &mut impl FnMut(i32) -> i32) -> i32 {
// CHECK-NEXT:         return drive(f, 2);
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut seen: i32 = 1;
// CHECK-NEXT:     let mut acc = move |x: i32| -> i32 {
// CHECK-NEXT:         seen = (seen * 2) + x;
// CHECK-NEXT:         return seen;
// CHECK-NEXT:     };
// CHECK-NEXT:     let d: i32 = crate::calc::again(&mut acc);
// CHECK-NEXT:     seen = 7;
// CHECK-NEXT:     let less = |a: i32, b: i32| -> bool {
// CHECK-NEXT:         return a < b;
// CHECK-NEXT:     };
// CHECK-NEXT:     let p: i32 = crate::calc::pick(&less, d, seen);
// CHECK-NEXT:     let mut log: Vec<i32> = Vec::new();
// CHECK-NEXT:     let mut note = |x: i32| {
// CHECK-NEXT:         log.push(x);
// CHECK-NEXT:     };
// CHECK-NEXT:     note(p);
// CHECK-NEXT:     note(d);
// CHECK-NEXT:     let zero = || -> u64 {
// CHECK-NEXT:         return 0u64;
// CHECK-NEXT:     };
// CHECK-NEXT:     print!("{} {} {} {}\n", d, p, log.len(), zero());
// CHECK-NEXT: }
//
// CPP:      #include <cstdio>
// CPP-NEXT: #include <vector>
// CPP-EMPTY:
// CPP-NEXT: namespace calc {
// CPP-EMPTY:
// CPP-NEXT: template <typename F> int pick(const F& f, const int a, const int b) {
// CPP-NEXT:     if (f(a, b)) {
// CPP-NEXT:         return a;
// CPP-NEXT:     }
// CPP-NEXT:     return b;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: template <typename F> int drive(F& f, const int times) {
// CPP-NEXT:     int last = 0;
// CPP-NEXT:     for (int i = 0; i < times; ++i) {
// CPP-NEXT:         last = f(i);
// CPP-NEXT:     }
// CPP-NEXT:     return last;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: template <typename F> int again(F& f) {
// CPP-NEXT:     return drive(f, 2);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace calc
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     int seen = 1;
// CPP-NEXT:     auto acc = [seen](const int x) mutable -> int {
// CPP-NEXT:         seen = (seen * 2) + x;
// CPP-NEXT:         return seen;
// CPP-NEXT:     };
// CPP-NEXT:     const int d = ::calc::again(acc);
// CPP-NEXT:     seen = 7;
// CPP-NEXT:     auto less = [](const int a, const int b) -> bool {
// CPP-NEXT:         return a < b;
// CPP-NEXT:     };
// CPP-NEXT:     const int p = ::calc::pick(less, d, seen);
// CPP-NEXT:     std::vector<int> log;
// CPP-NEXT:     auto note = [&log](const int x) {
// CPP-NEXT:         log.push_back(x);
// CPP-NEXT:     };
// CPP-NEXT:     note(p);
// CPP-NEXT:     note(d);
// CPP-NEXT:     auto zero = []() -> unsigned long long {
// CPP-NEXT:         return 0ULL;
// CPP-NEXT:     };
// CPP-NEXT:     std::printf("%d %d %zu %llu\n", d, p, log.size(), zero());
// CPP-NEXT:     return 0;
// CPP-NEXT: }
mod calc {
    pub fn pick(f: &impl Fn(i32, i32) -> bool, a: i32, b: i32) -> i32 {
        if f(a, b) {
            return a;
        }
        return b;
    }

    pub fn drive(f: &mut impl FnMut(i32) -> i32, times: i32) -> i32 {
        let mut last: i32 = 0;
        for i in 0..times {
            last = f(i);
        }
        return last;
    }

    pub fn again(f: &mut impl FnMut(i32) -> i32) -> i32 {
        return drive(f, 2);
    }
}

fn main() {
    let mut seen: i32 = 1;
    let mut acc = move |x: i32| -> i32 {
        seen = (seen * 2) + x;
        return seen;
    };
    let d: i32 = crate::calc::again(&mut acc);
    seen = 7;
    let less = |a: i32, b: i32| -> bool {
        return a < b;
    };
    let p: i32 = crate::calc::pick(&less, d, seen);
    let mut log: Vec<i32> = Vec::new();
    let mut note = |x: i32| {
        log.push(x);
    };
    note(p);
    note(d);
    let zero = || -> u64 {
        return 0u64;
    };
    print!("{} {} {} {}\n", d, p, log.len(), zero());
}
