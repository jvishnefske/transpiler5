// FR-124: the struct Copy decision is a module-level FIXPOINT over
// struct-typed fields, computed in the emitModule pre-pass -- hand-written
// dialect IR, ZERO new ops. The invariants pinned here, per shape:
//
// 1. ORDER INDEPENDENCE: the module lists consumers BEFORE producers
//    (@T uses @D uses @B, declared in that order), and the fixpoint still
//    resolves -- a single top-down pass could not.
// 2. ARRAY PEELING: @T's only field is `[D; 2]`; non-Copy-ness must reach
//    through the array type to the element struct ([T; N] is Copy iff T
//    is, so deriving Copy on @T would be the same rustc E0204).
// 3. BOTH ATTRIBUTE SEEDS PROPAGATE: has_drop (@B, W2.17) and
//    has_copy_ctor (@Q, W2.23) each strip Copy from the attributed struct
//    AND from every struct reaching it through fields (@D, @T, @DQ) --
//    the seeds live in one lambda in the pre-pass, so a trigger added
//    there covers derived classes for free.
// 4. THE POD CONTROL: @Pod, with no non-Copy field anywhere under it,
//    keeps the full `Clone, Copy, Default` derive byte-for-byte -- the
//    zero-golden-shift guarantee in miniature.
// 5. `Default` is UNTOUCHED everywhere: the fixpoint decides Copy only.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK: #[derive(Clone, Default)]
// CHECK-NEXT: struct T {
// CHECK-NEXT: inner: [D; 2],
emitrust.struct_def @T ["inner"] [!emitrust.array<2 x !emitrust.struct<"D">>]
// CHECK: #[derive(Clone, Default)]
// CHECK-NEXT: struct D {
emitrust.struct_def @D ["base", "weight"] [!emitrust.struct<"B">, f64]
// CHECK: #[derive(Clone, Default)]
// CHECK-NEXT: struct B {
emitrust.struct_def @B ["tag"] [i32] {emitrust.has_drop}
// CHECK: #[derive(Clone, Default)]
// CHECK-NEXT: struct Q {
emitrust.struct_def @Q ["v"] [i32] {emitrust.has_copy_ctor}
// CHECK: #[derive(Clone, Default)]
// CHECK-NEXT: struct DQ {
emitrust.struct_def @DQ ["base", "x"] [!emitrust.struct<"Q">, i32]
// CHECK: #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct Pod {
emitrust.struct_def @Pod ["a", "b"] [i32, i32]
