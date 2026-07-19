// FR: UNSUPPORTED-COVERAGE is a SOFT SKIP, never an error — load-bearing
// rule of the value-identity checker. The concrete interpreter covers a
// deliberately bounded op set; when an entry point reaches ANYTHING else
// (here: an emitrust.call_opaque other than "print!", whose result is
// data-flow live so it cannot be ignored), interpretation of THAT entry
// point aborts SILENTLY: no diagnostic, the pass still succeeds, and only
// an internal pass statistic counts the skip. (The statistic is not pinned
// here: llvm::Statistic is compiled out of release builds.)
//
// Skipping is per ENTRY POINT, not per module: @ok below has full coverage
// and must still be interpreted + compared. Exit 0, module round-trips
// unchanged.
//
// RUN: emitrust-opt --emitrust-value-identity-check %s | FileCheck %s

// CHECK-LABEL: func.func @mystery
// CHECK: emitrust.call_opaque "unknown!"
// CHECK: return
func.func @mystery() -> i32 {
  %c1_i32 = arith.constant 1 : i32
  emitrust.call_opaque "print!"(%c1_i32) {args = ["{}\0A", 0 : index]} : (i32) -> ()
  // Uninterpretable: opaque call with a live result. This entry point must
  // abort as unsupported coverage WITHOUT any diagnostic.
  %x = emitrust.call_opaque "unknown!"(%c1_i32) : (i32) -> i32
  emitrust.call_opaque "print!"(%x) {args = ["{}\0A", 0 : index]} : (i32) -> ()
  return %x : i32
}

// CHECK-LABEL: func.func @ok
// CHECK: emitrust.call_opaque "print!"
// CHECK: return
func.func @ok() -> i32 {
  %c0_i32 = arith.constant 0 : i32
  %c9_i32 = arith.constant 9 : i32
  %s = arith.addi %c9_i32, %c9_i32 : i32
  emitrust.call_opaque "print!"(%s) {args = ["{}\0A", 0 : index]} : (i32) -> ()
  return %c0_i32 : i32
}
