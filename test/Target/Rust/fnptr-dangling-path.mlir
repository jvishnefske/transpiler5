// FR-159 / FR-77: the dangling-fn-ptr-target backstop must fire on BOTH
// spellings of a fn-item target -- the flat `tu0_nope` and the
// path-qualified `crate::tu0::nope`.
//
// Why this file exists: the backstop's parse-back predicate accepted only
// IDENTIFIER-shaped payload text (alphanumerics and underscores), so the
// instant FR-159 gave items `crate::<module>::<leaf>` symbols, every
// path-spelled `Some(..)` payload fell out of its reach. Measured at HEAD:
// `Some(tu0_nope)` was refused with a located diagnostic while
// `Some(crate::tu0::nope)` produced NO diagnostic at all and was emitted
// into the crate -- rustc E0433/E0425 at best, and a rejection-is-a-feature
// violation either way. A safety backstop that silently switches itself off
// for the new spelling is worse than not having one, so both spellings are
// pinned here and neither may regress without this file failing.
//
// The complement -- a payload naming a function the module DOES define --
// is what module-items.mlir renders; it must stay silent, and the last two
// cases below pin that in both spellings too.
// RUN: not emitrust-translate --mlir-to-rust --split-input-file %s 2>&1 | FileCheck %s

// The historical flat spelling.
// CHECK: dangling function pointer target 'tu0_nope': the module defines no function with that name
emitrust.global @TU0_CB <#emitrust.opaque<"Some(tu0_nope)">> : !emitrust.fn_ptr<(i32) -> i32>

// -----

// The FR-159 path spelling: same defect, same located diagnostic, and the
// diagnostic names the WHOLE path so the reader can find the missing item.
// CHECK: dangling function pointer target 'crate::tu0::nope': the module defines no function with that name
emitrust.global @"crate::tu0::CB" <#emitrust.opaque<"Some(crate::tu0::nope)">> : !emitrust.fn_ptr<(i32) -> i32>

// -----

// The aggregate position (a fn-ptr TABLE, the systemd rlimit-util shape)
// with a path payload: the walk recurses, so a dangling leaf is caught
// there too.
// CHECK: dangling function pointer target 'crate::tu0::op_a': the module defines no function with that name
emitrust.global const @"crate::tu0::OPS" <[#emitrust.opaque<"Some(crate::tu0::op_a)">, #emitrust.opaque<"None">]> : !emitrust.array<2x!emitrust.fn_ptr<(i32) -> i32>>

// -----

// The rvalue position inside a function body.
// CHECK: dangling function pointer target 'crate::tu1::missing': the module defines no function with that name
emitrust.func @use_missing() -> !emitrust.fn_ptr<(i32) -> i32> {
  %0 = emitrust.constant <#emitrust.opaque<"Some(crate::tu1::missing)">> : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.return %0 : !emitrust.fn_ptr<(i32) -> i32>
}

// -----

// A path payload naming a function the module DOES define is silent -- and
// so is the flat one. `not` above requires SOME leg to fail, which the four
// cases above supply; these two must contribute no diagnostic, which
// CHECK-NOT below enforces over the whole stderr.
// CHECK-NOT: dangling function pointer target 'crate::tu2::present'
// CHECK-NOT: dangling function pointer target 'tu2_present'
emitrust.func @"crate::tu2::present"(%arg0: i32) -> i32 {
  emitrust.return %arg0 : i32
}
emitrust.func @tu2_present(%arg0: i32) -> i32 {
  emitrust.return %arg0 : i32
}
emitrust.global @"crate::tu2::OK" <#emitrust.opaque<"Some(crate::tu2::present)">> : !emitrust.fn_ptr<(i32) -> i32>
emitrust.global @TU2_OK <#emitrust.opaque<"Some(tu2_present)">> : !emitrust.fn_ptr<(i32) -> i32>
