// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// An open enum whose variants share values (a C `enum { A = 1, B = 1 }`)
// lowers to distinct associated consts of equal value -- valid Rust that
// preserves `A == B` under the derived `PartialEq`. The dialect verifier
// accepts equal variant values; only variant NAMES must be unique.
emitrust.enum_def @E ["A", "B", "C", "D"] [1, 1, 2, 2]

// CHECK:      struct E(i32);
// CHECK:      impl E {
// CHECK-NEXT:     const A: E = E(1);
// CHECK-NEXT:     const B: E = E(1);
// CHECK-NEXT:     const C: E = E(2);
// CHECK-NEXT:     const D: E = E(2);
// CHECK-NEXT: }
