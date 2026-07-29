// RUN: not emitrust-cc --emit=import %s -o /dev/null 2>&1 | FileCheck %s
// RUN: not emitrust-cc --emit=rust %s -o /dev/null 2>&1 | FileCheck %s
// RUN: emitrust-cc --recover --emit=import %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=RECOVER

// FR-42 negative gate: recoverable import is OFF by default and changes
// NOTHING when it is off. The very input that recover-skip-unsupported.c
// transpiles partially still fails the whole compile here, with the same
// wording, the same location, and the same nonzero exit code it has always
// had — no warning, no partial module, no ledger output.
//
// This is the per-test half of the behavior-preservation evidence; the
// whole-corpus half is a byte-identical --emit=rust snapshot over
// test/EndToEnd, which the flag must also leave untouched.

int supported(int x) { return x + 1; }

int unsupported(int x) {
  volatile int v = x;
  return v;
}

int main(void) { return supported(1) + unsupported(2); }

// CHECK: recover-off-by-default.c:19:16: error: unsupported: volatile-qualified type
// CHECK-NOT: warning:
// CHECK-NOT: recovered
// CHECK-NOT: blocker tabulation

// RECOVER: warning: unsupported: volatile-qualified type
// RECOVER-NOT: error:
