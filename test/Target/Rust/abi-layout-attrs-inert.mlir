// FR-182: the C-ABI faithfulness verdict and clang's layout numbers ride the
// `emitrust.struct_def` as DISCARDABLE attributes, and they must be inert
// everywhere except the `--c-abi-exports` decision.
//
// Two properties are pinned, and both are load-bearing.
//
// 1. ROUND-TRIP. The attributes survive `emitrust-opt` unchanged -- no new op
//    and no new type was introduced for FR-182 precisely so that nothing in
//    the pass pipeline, the dialect verifier or the shard serializer had to
//    learn about them. Printed, reparsed and printed again, byte for byte.
//
// 2. INERTNESS. `emitrust-translate --mlir-to-rust` has no export mode, so a
//    module carrying these attributes must emit exactly what the same module
//    without them emits: no `#[repr(C)]`, no `const _: () = assert!(..)`, no
//    wrapper. The flag defaults OFF and this is the emitter-level half of
//    that promise (the driver-level half, over a whole crate root, is
//    test/Driver/c-abi-exports-structs.c's diff).
//
// The `#[repr(C)]` promise is made ONLY for a struct something actually
// crosses the boundary with. A faithful struct that nothing exports is an
// ordinary struct, and giving it the C layout would be a behavior change to
// every crate this project has ever emitted.
//
// RUN: emitrust-opt %s | emitrust-opt | FileCheck %s --check-prefix=ROUNDTRIP
// RUN: emitrust-translate --mlir-to-rust %s -o - \
// RUN:   | FileCheck %s --check-prefix=RUST
// RUN: emitrust-translate --mlir-to-rust %s -o - > %t.attrs.rs
// RUN: not grep 'repr(C)' %t.attrs.rs
// RUN: not grep 'const _: ()' %t.attrs.rs
// RUN: not grep export_name %t.attrs.rs
// RUN: not grep 'extern "C"' %t.attrs.rs

module {
  // ROUNDTRIP: emitrust.struct_def @Pt ["x", "y"] [i32, i32] {emitrust.abi_faithful, emitrust.abi_layout = {align = 4 : i64, offsets = [0, 4], size = 8 : i64}}
  emitrust.struct_def @Pt ["x", "y"] [i32, i32] {emitrust.abi_faithful, emitrust.abi_layout = {align = 4 : i64, offsets = [0, 4], size = 8 : i64}}
  // ROUNDTRIP: emitrust.struct_def @Bw ["val", "buffer"] [ui64, i64] {emitrust.abi_unfaithful_reason = "the pointer member 'buffer', emitted as an i64 data-pointer cursor rather than an address"}
  emitrust.struct_def @Bw ["val", "buffer"] [ui64, i64] {emitrust.abi_unfaithful_reason = "the pointer member 'buffer', emitted as an i64 data-pointer cursor rather than an address"}
}

// The rendered items are the historical ones, attributes or no attributes.
// RUST:      #[derive(Clone, Copy, Default)]
// RUST-NEXT: struct Pt {
// RUST-NEXT:     x: i32,
// RUST-NEXT:     y: i32,
// RUST-NEXT: }
// RUST-NEXT: #[derive(Clone, Copy, Default)]
// RUST-NEXT: struct Bw {
// RUST-NEXT:     val: u64,
// RUST-NEXT:     buffer: i64,
// RUST-NEXT: }
