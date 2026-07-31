// FR-55: a struct whose fields the `Default` derive cannot cover is emitted
// with an explicit `impl Default` instead. Rust's blanket
// `impl<T: Default> Default for [T; N]` stops at N = 32, so a longer array
// field makes `#[derive(Default)]` itself fail to compile; the explicit impl
// names every field with the value the derive would have given it.
// The derive is KEPT everywhere it works, including at the N = 32 boundary.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// The boundary length is still derivable.
// CHECK:      #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct AtLimit {
// CHECK-NEXT:     buf: [u8; 32],
// CHECK-NEXT: }
// CHECK-NOT:  impl Default for AtLimit
emitrust.struct_def @AtLimit ["buf"] [!emitrust.array<32xui8>]

// One past it is not: mixed field types, all defaulted independently.
// CHECK:      #[derive(Clone, Copy)]
// CHECK-NEXT: struct Oversized {
// CHECK-NEXT:     n: i32,
// CHECK-NEXT:     buf: [u8; 33],
// CHECK-NEXT:     x: f64,
// CHECK-NEXT: }
// CHECK-NEXT: impl Default for Oversized {
// CHECK-NEXT:     fn default() -> Oversized {
// CHECK-NEXT:         Oversized {
// CHECK-NEXT:             n: 0,
// CHECK-NEXT:             buf: [0; 33],
// CHECK-NEXT:             x: 0.0,
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT: }
emitrust.struct_def @Oversized ["n", "buf", "x"] [i32, !emitrust.array<33xui8>, f64]

// Nesting is looked through: the OUTER extent is within the limit, but the
// inner one is not, so `[[u32; 68]; 2]` has no derived `Default` either.
// CHECK:      #[derive(Clone, Copy)]
// CHECK-NEXT: struct NestedArray {
// CHECK-NEXT:     rk: {{\[\[}}u32; 68]; 2],
// CHECK-NEXT: }
// CHECK-NEXT: impl Default for NestedArray {
// CHECK-NEXT:     fn default() -> NestedArray {
// CHECK-NEXT:         NestedArray {
// CHECK-NEXT:             rk: {{\[\[}}0; 68]; 2],
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT: }
emitrust.struct_def @NestedArray ["rk"] [!emitrust.array<2x!emitrust.array<68xui32>>]

// A function-pointer element defaults to `None`, matching the derive.
// CHECK:      #[derive(Clone, Copy)]
// CHECK-NEXT: struct Table {
// CHECK-NEXT:     ops: [Option<fn(i32) -> i32>; 64],
// CHECK-NEXT: }
// CHECK-NEXT: impl Default for Table {
// CHECK-NEXT:     fn default() -> Table {
// CHECK-NEXT:         Table {
// CHECK-NEXT:             ops: [None; 64],
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT: }
emitrust.struct_def @Table ["ops"] [!emitrust.array<64x!emitrust.fn_ptr<(i32) -> i32>>]

// An array OF the oversized struct is short enough to derive: `Oversized`
// carries its own `Default`, so the recursion stops at the named struct and
// the containing field is covered again.
// CHECK:      #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct HoldsOversized {
// CHECK-NEXT:     one: Oversized,
// CHECK-NEXT:     many: [Oversized; 4],
// CHECK-NEXT: }
// CHECK-NOT:  impl Default for HoldsOversized
emitrust.struct_def @HoldsOversized ["one", "many"]
    [!emitrust.struct<"Oversized">, !emitrust.array<4x!emitrust.struct<"Oversized">>]

// ... but a LONG array of it is not, and its element default is the struct's
// own `Default` (which is `Copy`, so the repeat expression is well formed).
// CHECK:      #[derive(Clone, Copy)]
// CHECK-NEXT: struct ManyOversized {
// CHECK-NEXT:     slots: [Oversized; 40],
// CHECK-NEXT: }
// CHECK-NEXT: impl Default for ManyOversized {
// CHECK-NEXT:     fn default() -> ManyOversized {
// CHECK-NEXT:         ManyOversized {
// CHECK-NEXT:             slots: [Oversized::default(); 40],
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT: }
emitrust.struct_def @ManyOversized ["slots"]
    [!emitrust.array<40x!emitrust.struct<"Oversized">>]
