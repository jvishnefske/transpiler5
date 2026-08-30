// FR-175: `emitrust.switch` must accept `i64::MAX` and `i64::MIN` as case
// values. Its verifier collected the case values in an
// `llvm::DenseSet<int64_t>`, whose EMPTY KEY is `i64::MAX` and whose
// TOMBSTONE KEY is `i64::MIN`, so a case at either sentinel is
// indistinguishable from an unoccupied bucket. This is the same defect
// FR-170 fenced off in MLIR's own `scf::IndexSwitchOp::verify`; the entry
// that filed it assumed we had inherited the identical symptom, and we had
// not -- so this file pins what was actually MEASURED, not what was filed.
//
// The trigger is ORDER-DEPENDENT, which is why every position appears below.
// A default-constructed `DenseMap` has ZERO buckets and `LookupBucketFor`
// early-returns "not found" for any key at all, so `i64::MAX` inserted FIRST
// succeeds and looks fine. Inserted after ANY other value it probes a real
// bucket array, matches an empty bucket by value, and is reported as a
// duplicate that the switch does not have:
//   case 9223372036854775807 alone            -- accepted before the fix
//   case 1, case 9223372036854775807          -- FALSE "duplicate case value"
// `i64::MIN` never fired, because nothing is ever erased from this set and
// so no bucket ever holds the tombstone -- but it is one `erase` away from
// the same false report, and in an assertions-enabled build BOTH sentinels
// abort inside `LookupBucketFor` rather than diagnosing anything. Both are
// pinned here so neither can regress quietly.
//
// The verdict this protects is a POSITIVE one -- "this IR is fine" -- so the
// oracle has to be a clean round-trip, not a diagnostic. The negative leg
// lives in invalid.mlir and is deliberately NOT weakened: a real duplicate
// must still be refused, including a real duplicate AT a sentinel.

// RUN: emitrust-opt %s | emitrust-opt | FileCheck %s

// CHECK-LABEL: emitrust.func @sentinel_max_only
// CHECK: case 9223372036854775807 {
emitrust.func @sentinel_max_only(%arg0: i64) {
  emitrust.switch %arg0 : i64
  case 9223372036854775807 {
  }
  default {
  }
  emitrust.return
}

// The order that actually reproduced: a sentinel case reached with the
// bucket array already allocated.
// CHECK-LABEL: emitrust.func @sentinel_max_after
// CHECK: case 1 {
// CHECK: case 9223372036854775807 {
emitrust.func @sentinel_max_after(%arg0: i64) {
  emitrust.switch %arg0 : i64
  case 1 {
  }
  case 9223372036854775807 {
  }
  default {
  }
  emitrust.return
}

// CHECK-LABEL: emitrust.func @sentinel_min_after
// CHECK: case -9223372036854775808 {
emitrust.func @sentinel_min_after(%arg0: i64) {
  emitrust.switch %arg0 : i64
  case 1 {
  }
  case -9223372036854775808 {
  }
  default {
  }
  emitrust.return
}

// Both sentinels in one switch, with an ordinary value between them, so the
// fix cannot be a special case for whichever one happens to come first.
// CHECK-LABEL: emitrust.func @both_sentinels
// CHECK: case 9223372036854775807 {
// CHECK: case 0 {
// CHECK: case -9223372036854775808 {
emitrust.func @both_sentinels(%arg0: i64) {
  emitrust.switch %arg0 : i64
  case 9223372036854775807 {
  }
  case 0 {
  }
  case -9223372036854775808 {
  }
  default {
  }
  emitrust.return
}
