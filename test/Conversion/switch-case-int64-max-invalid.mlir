// FR-170: the i64::MAX switch-label fence at IR level, and the proof that it
// is exactly ONE value wide.
//
// `lift-cf-to-scf` turns a `cf.switch` into an `scf.index_switch` whose
// `cases` are the labels' ZERO-extended values stored as `int64_t`
// (mlir/lib/Conversion/ControlFlowToSCF/ControlFlowToSCF.cpp uses
// `apInt.getZExtValue()`). `scf::IndexSwitchOp::verify` then puts them in a
// `DenseSet<int64_t>`, whose EMPTY key is `i64::MAX` -- so and only so, the
// single value `i64::MAX` comes back from `insert()` as an already-present
// duplicate and the whole compile dies on a claim that is false.
//
// The fence runs BEFORE the lift and rejects only that one value. The two
// neighbours a naive "reserved keys" fence would also swallow are the
// controls here and must pass through UNCHANGED:
//   * `i64::MIN`, which is the DenseMapInfo tombstone for a signed integral
//     that is not `long`; and
//   * `i64::MAX - 1`, which IS the tombstone for `int64_t` on LP64 (where
//     `int64_t` is `long`) and still round-trips through the set.
// test/EndToEnd/switch-case-int64-boundary.c byte-diffs both of them against
// the clang-built native, because "the pass did not reject it" is not
// evidence that the emitted code is right.
//
// RUN: not emitrust-opt %s --emitrust-reject-unrepresentable-switch-cases \
// RUN:   --split-input-file 1>%t.out 2>%t.err
// RUN: FileCheck --check-prefix=KEPT %s < %t.out
// RUN: FileCheck --check-prefix=ERR %s < %t.err

// The i64::MIN control: accepted, and passed through byte-for-byte. It is
// printed back as `9223372036854775808` because the custom cf.switch printer
// spells labels UNSIGNED -- that is the separate FR-135 defect, and it is
// deliberately not hidden here.
// KEPT-LABEL: func.func @at_min
// KEPT: cf.switch
// KEPT: 9223372036854775808: ^bb
func.func @at_min(%arg0: i64) -> i32 {
  cf.switch %arg0 : i64, [
    default: ^bb2,
    -9223372036854775808: ^bb1
  ]
^bb1:
  %c1_i32 = arith.constant 1 : i32
  return %c1_i32 : i32
^bb2:
  %c0_i32 = arith.constant 0 : i32
  return %c0_i32 : i32
}

// -----

// The i64::MAX-1 control -- the LP64 tombstone key, one below the rejected
// value. Accepted, and passed through byte-for-byte.
// KEPT-LABEL: func.func @at_max_minus_one
// KEPT: cf.switch
// KEPT: 9223372036854775806: ^bb
func.func @at_max_minus_one(%arg0: i64) -> i32 {
  cf.switch %arg0 : i64, [
    default: ^bb2,
    9223372036854775806: ^bb1
  ]
^bb1:
  %c1_i32 = arith.constant 1 : i32
  return %c1_i32 : i32
^bb2:
  %c0_i32 = arith.constant 0 : i32
  return %c0_i32 : i32
}

// -----

// The one rejected value.
func.func @at_max(%arg0: i64) -> i32 {
  // ERR: error: unsupported: switch case value 9223372036854775807 is i64::MAX, which the upstream 'scf.index_switch' verifier cannot represent
  // ERR: note: MLIR's scf::IndexSwitchOp::verify collects case values in a DenseSet<int64_t> whose EMPTY KEY is i64::MAX, so the value is reported as a duplicate even when the switch has none
  cf.switch %arg0 : i64, [
    default: ^bb2,
    9223372036854775807: ^bb1
  ]
^bb1:
  %c1_i32 = arith.constant 1 : i32
  return %c1_i32 : i32
^bb2:
  %c0_i32 = arith.constant 0 : i32
  return %c0_i32 : i32
}

// The false claim must not appear anywhere on this path.
// ERR-NOT: duplicate case value
// KEPT-NOT: @at_max(
