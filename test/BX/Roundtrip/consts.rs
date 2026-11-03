// FR-264: Rust constants -> C++, law 2 at the CLI from the Rust side (the
// C++ printed for this file, fed back in, prints byte-identical C++; its
// Rust is this file's canonical Rust). Pinned: `const k: T = e;` at the root
// is `const T k = e;`, `pub const` in a `pub mod` is a namespace member and
// a private `const` an anonymous-namespace member; `crate::k` / `crate::a::k`
// and a bare name of the SAME module resolve to one absolute constant (Rust
// can never bind a local of a constant's name in scope, so a bare `kBase`
// always means the constant), while a local `kBase` inside `mod util` -- where
// no constant `kBase` is in scope -- stays a local and prints next to `::kBase`;
// a FUNCTION may use a constant defined later (Rust accepts it and the C++
// constants block precedes every function).
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: %emitrust-bx --emit=cpp %t.cpp -o %t.2.cpp
// RUN: diff %t.cpp %t.2.cpp
// RUN: %emitrust-bx --emit=rust %t.cpp -o %t.rs
// RUN: %emitrust-bx --emit=rust %s -o %t.self.rs
// RUN: diff %t.rs %t.self.rs
// RUN: %emitrust-bx --check-roundtrip --emit=rust %s | FileCheck %s
// RUN: FileCheck --check-prefix=CPP %s --input-file=%t.cpp
#![allow(non_upper_case_globals)]

fn early() -> u32 {
    return kLimit.wrapping_mul(3u32);
}

const kBase: i32 = 10;

const kLimit: u32 = 4000000000u32;

mod util {
    pub const kStep: i32 = crate::kBase / 4;

    const kHidden: i32 = kStep + 1;

    pub fn next(x: i32) -> i32 {
        let kBase: i32 = 3;
        return x + kHidden + kBase + crate::kBase;
    }
}

fn main() {
    let mut x: i32 = 0;
    for i in 0..kBase {
        x = crate::util::next(x) + i;
    }
    print!("{} {} {}\n", x, early(), crate::util::kStep);
}

// CHECK:      #![allow(non_upper_case_globals)]
// CHECK-EMPTY:
// CHECK-NEXT: const kBase: i32 = 10;
// CHECK-EMPTY:
// CHECK-NEXT: const kLimit: u32 = 4000000000u32;
// CHECK-EMPTY:
// CHECK-NEXT: mod util {
// CHECK-NEXT:     pub const kStep: i32 = crate::kBase / 4;
// CHECK-EMPTY:
// CHECK-NEXT:     const kHidden: i32 = kStep + 1;
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn next(x: i32) -> i32 {
// CHECK-NEXT:         let kBase: i32 = 3;
// CHECK-NEXT:         return x + kHidden + kBase + crate::kBase;
// CHECK:      fn early() -> u32 {
// CHECK-NEXT:     return kLimit.wrapping_mul(3u32);

// CPP:      const int kBase = 10;
// CPP-NEXT: const unsigned kLimit = 4000000000U;
// CPP-NEXT: namespace util {
// CPP-NEXT: const int kStep = ::kBase / 4;
// CPP-NEXT: namespace {
// CPP-NEXT: const int kHidden = kStep + 1;
// CPP-NEXT: }  // namespace
// CPP-NEXT: }  // namespace util
// CPP:      int next(const int x) {
// CPP-NEXT:     const int kBase = 3;
// CPP-NEXT:     return x + kHidden + kBase + ::kBase;
// CPP:      unsigned early() {
// CPP-NEXT:     return kLimit * 3U;
