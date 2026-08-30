// FR-170: the diagnostic for a `case i64::MAX:` label must be TRUE.
//
// Before this fence the program below was rejected with
//
//   error: 'scf.index_switch' op has duplicate case value: 9223372036854775807
//
// and there is no duplicate. The switch has four distinct labels. The
// message is a pure artifact of an UPSTREAM implementation detail we cannot
// patch: `scf::IndexSwitchOp::verify` (mlir/lib/Dialect/SCF/IR/SCF.cpp)
// collects the labels in a `DenseSet<int64_t>`, and
// `DenseMapInfo<T>::getEmptyKey()` for an integral `T` is
// `std::numeric_limits<T>::max()` (llvm/include/llvm/ADT/DenseMapInfo.h).
// Inserting i64::MAX therefore probes a bucket that compares equal to the
// EMPTY key, `insert().second` comes back false, and the verifier reports a
// duplicate that does not exist.
//
// We cannot fix upstream, but we must stop lying about it: a user who reads
// "duplicate case value" goes looking for a second `case` label and finds
// nothing. So the pipeline fences the value BEFORE `lift-cf-to-scf` can
// build the `scf.index_switch`, and says what is actually wrong, at the
// switch. Deliberately NOT worked around by shifting or remapping the label:
// that would be a silent representation change on the correctness-sensitive
// path, and this project's rule is that rejection is a feature.
//
// The fence is exactly ONE value wide -- i64::MIN and i64::MAX-1 (the
// DenseSet TOMBSTONE key, since `int64_t` is `long` on LP64 and its
// tombstone is `max()-1`) still compile, and
// test/EndToEnd/switch-case-int64-boundary.c byte-diffs both against the
// clang-built native.
//
// RUN: not emitrust-cc --emit=rust %s -o - 2>&1 | FileCheck %s

int f(long long t) {
  // CHECK: switch-case-int64-max-reject.c:[[#@LINE+1]]:3: error: unsupported: switch case value 9223372036854775807 is i64::MAX, which the upstream 'scf.index_switch' verifier cannot represent
  switch (t) {
  case 0:
    return 10;
  case 5000000000LL:
    return 12;
  case 9223372036854775807LL:
    return 13;
  default:
    return 99;
  }
}

// The note carries the WHY, so its wording is pinned too: without it the
// error is honest but unactionable.
// CHECK: note: MLIR's scf::IndexSwitchOp::verify collects case values in a DenseSet<int64_t> whose EMPTY KEY is i64::MAX, so the value is reported as a duplicate even when the switch has none

// The false claim must never come back.
// CHECK-NOT: duplicate case value

int main(void) { return f(1); }
