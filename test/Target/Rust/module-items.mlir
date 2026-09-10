// FR-159 phase 1: per-item Rust MODULE emission. A module-level item whose
// SYMBOL is the absolute Rust path `crate::<module>::<leaf>` renders inside
// `mod <module> { ... }` under its LEAF name; an item whose symbol carries no
// path keeps the historical flat rendering, in its historical position, byte
// for byte. This is the emitter half of the link-time shape-conflict sink:
// two translation-unit-local C types sharing one C tag become two PATHS
// instead of one name, so a link that used to be refused now succeeds.
//
// What this file pins, and why each part is load-bearing:
//   (a) only the DEFINITION side changes -- USE sites (call_opaque callees,
//       global loads, struct types, struct literals, `T::default()`) already
//       spell the full path with no emitter change at all, so the CHECKs
//       below deliberately show ROOT items naming MODULE items;
//   (b) paths are ABSOLUTE (`crate::tu0::add1`, never `tu0::add1`). A
//       relative path is module-relative inside `mod tu0` and rustc rejects
//       it with E0433 -- and a fn-ptr TABLE payload renders INSIDE the
//       module, so this is the common case, not a corner one;
//   (c) visibility inside a module is uniformly `pub(crate)`, on the item
//       AND on a struct's FIELDS: root constructors name those fields and a
//       private field is rustc E0616;
//   (d) FR-63's `const { ... }` wrapper survives the path spelling. The
//       predicate deciding it filtered on identifier-shaped text and
//       silently no-opped on anything containing ':', which would have
//       dropped the wrapper -- an emitted-byte change plus a
//       clippy::missing_const_for_thread_local regression, with nothing to
//       notice it;
//   (e) two structs with the SAME leaf name, one at the root and one in a
//       module, coexist. That is the entire point of the sink.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s --strict-whitespace

// The root items come first, in their historical order and rendering.
// FR-220: "historical rendering" now includes a targeted `#[allow(dead_code)]`
// ahead of every record and every associated-constant `impl`, replacing the
// crate root's retired blanket `#![allow(dead_code)]`. It is attribute
// POSITION only, and this file is the sink's byte-level pin: the attribute is
// emitted INSIDE the `mod` at the module's own indentation, so the
// `--strict-whitespace` CHECK-NEXT chains are what prove the sink did not
// reflow anything.
// CHECK:      #[allow(dead_code)]
// CHECK-NEXT: #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct Buf {
// CHECK-NEXT:     a: i32,
// CHECK-NEXT: }
emitrust.struct_def @Buf ["a"] [i32]

// A ROOT global of a MODULE struct type: the type and its `default()` both
// spell the absolute path, with no emitter change.
// CHECK:      thread_local! {
// CHECK-NEXT:     static ORIGIN: std::cell::Cell<crate::tu0::Buf> = std::cell::Cell::new(crate::tu0::Buf::default());
// CHECK-NEXT: }
emitrust.global @ORIGIN : !emitrust.struct<"crate::tu0::Buf">

// A ROOT function calling a MODULE function and reading a MODULE global.
// CHECK:      fn root_use(v: i32) -> i32 {
// CHECK-NEXT:     let v0: i32 = crate::tu0::add1(v);
// CHECK-NEXT:     v0 + crate::tu0::TOTAL.with(|__emitrust_tl| __emitrust_tl.get())
// CHECK-NEXT: }
emitrust.func @root_use(%arg0: i32) -> i32 attributes {emitrust.param_names = ["v"]} {
  %0 = emitrust.call_opaque "crate::tu0::add1"(%arg0) : (i32) -> i32
  %1 = emitrust.global_load @"crate::tu0::TOTAL" : i32
  %2 = emitrust.add %0, %1 : i32
  emitrust.return %2 : i32
}

// ... and then ONE `mod` per distinct path, as an epilogue, in
// first-appearance order.
// CHECK:      mod tu0 {
// CHECK-NEXT:     pub(crate) fn add1(v: i32) -> i32 {
// CHECK-NEXT:         v + 1i32
// CHECK-NEXT:     }
emitrust.func @"crate::tu0::add1"(%arg0: i32) -> i32 attributes {emitrust.param_names = ["v"]} {
  %0 = emitrust.constant <1 : i32> : i32
  %1 = emitrust.add %arg0, %0 : i32
  emitrust.return %1 : i32
}

// (c)+(e): the module's own `Buf`, a DIFFERENT shape from the root's, with
// `pub(crate)` on the struct and on every field.
// CHECK-NEXT:     #[allow(dead_code)]
// CHECK-NEXT:     #[derive(Clone, Copy, Default)]
// CHECK-NEXT:     pub(crate) struct Buf {
// CHECK-NEXT:         pub(crate) a: i32,
// CHECK-NEXT:         pub(crate) b: i32,
// CHECK-NEXT:     }
emitrust.struct_def @"crate::tu0::Buf" ["a", "b"] [i32, i32]

// (b)+(d): the fn-ptr table payload is ABSOLUTE even though it renders
// inside `mod tu0`, and it keeps its `const { ... }` wrapper.
// CHECK-NEXT:     thread_local! {
// CHECK-NEXT:         pub(crate) static TABLE: std::cell::Cell<[Option<fn(i32) -> i32>; 1]> = const { std::cell::Cell::new([Some(crate::tu0::add1)]) };
// CHECK-NEXT:     }
emitrust.global @"crate::tu0::TABLE" <[#emitrust.opaque<"Some(crate::tu0::add1)">]> : !emitrust.array<1x!emitrust.fn_ptr<(i32) -> i32>>

// CHECK-NEXT:     thread_local! {
// CHECK-NEXT:         pub(crate) static TOTAL: std::cell::Cell<i32> = const { std::cell::Cell::new(7) };
// CHECK-NEXT:     }
emitrust.global @"crate::tu0::TOTAL" <7 : i32> : i32

// CHECK-NEXT:     pub(crate) static LIMIT: i32 = 9;
emitrust.global const @"crate::tu0::LIMIT" <9 : i32> : i32

// An enum in a module: the leaf name carries through the newtype, its
// associated constants and its Default impl.
// CHECK-NEXT:     #[repr(transparent)]
// CHECK-NEXT:     #[derive(Clone, Copy, PartialEq)]
// CHECK-NEXT:     #[allow(dead_code)]
// CHECK-NEXT:     pub(crate) struct Mode(pub(crate) i32);
// CHECK-NEXT:     #[allow(dead_code)]
// CHECK-NEXT:     impl Mode {
// CHECK-NEXT:         pub(crate) const RAW: Mode = Mode(0);
// CHECK-NEXT:         pub(crate) const SCALED: Mode = Mode(1);
// CHECK-NEXT:     }
// CHECK-NEXT:     impl Default for Mode {
// CHECK-NEXT:         fn default() -> Mode { Mode::RAW }
// CHECK-NEXT:     }
// CHECK-NEXT: }
emitrust.enum_def @"crate::tu0::Mode" ["RAW", "SCALED"] [0, 1]

// A SECOND path gets its OWN module, in first-appearance order; its `add1`
// shares the ROOT-visible leaf name with tu0's and stays distinct.
// CHECK-NEXT: mod tu1 {
// CHECK-NEXT:     pub(crate) fn add1(v: i32) -> i32 {
// CHECK-NEXT:         v * 10i32
// CHECK-NEXT:     }
// CHECK-NEXT: }
emitrust.func @"crate::tu1::add1"(%arg0: i32) -> i32 attributes {emitrust.param_names = ["v"]} {
  %0 = emitrust.constant <10 : i32> : i32
  %1 = emitrust.mul %arg0, %0 : i32
  emitrust.return %1 : i32
}
