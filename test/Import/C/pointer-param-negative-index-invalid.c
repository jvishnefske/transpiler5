// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/const-index.c 2>&1 | FileCheck %s --check-prefix=CONST
// RUN: not emitrust-import-c %t/const-index-2.c 2>&1 | FileCheck %s --check-prefix=CONST2
// RUN: not emitrust-import-c %t/deref-minus.c 2>&1 | FileCheck %s --check-prefix=DEREF
// RUN: not emitrust-import-c %t/three-objects.c 2>&1 | FileCheck %s --check-prefix=THREE
// RUN: not emitrust-import-c %t/write-back.c 2>&1 | FileCheck %s --check-prefix=WRITE
// RUN: not emitrust-import-c %t/cursor-local.c 2>&1 | FileCheck %s --check-prefix=CURSOR
// RUN: not emitrust-import-c %t/decrement.c 2>&1 | FileCheck %s --check-prefix=DECR

// Intent: pin the located refusal of a NEGATIVE element index below a
// Phase-1b slice parameter's origin, and pin it as a FLIP of a runtime
// miscompile rather than as a new restriction.
//
// The importer has two pointer lowerings and they disagree here. When a
// pointer parameter's class unifies to exactly ONE local storage base,
// `planOwners` promotes it to an owner method whose parameter is an i64
// element CURSOR into the receiver's whole data array, so `p[-1]` adds -1
// to the cursor in i64 and reaches a real element (pinned end-to-end in
// test/EndToEnd/pointer-param-negative-index-owner.c). With TWO or more
// storage bases the class stays on the Phase-1b slice lowering: the call
// site re-bases the slice at the pointee (`&mut a[1..]`) and the callee's
// own cursor restarts at 0, so the elements before the pointee are simply
// not in the slice.
//
// Measured at 931c44f, every file below emitted, `cargo build`-ed clean,
// and then PANICKED at run time -- e.g. `index out of bounds: the len is 2
// but the index is 18446744073709551615` for const-index.c, whose native
// prints `7 1`. Each access here is IN BOUNDS in C (except three-objects.c
// and write-back.c, which are equally in bounds), so that panic was a
// wrong ANSWER on well-defined C, not a safe failure -- and the crate built
// cleanly, so no compile-time oracle could see it. A located refusal is the
// floor: it moves the failure to import time where it is honest.
//
// Only a PROVABLY negative cursor is refused. A runtime index through a
// slice parameter (`p[k]`) is unchanged, so this fence costs no shape the
// importer could already prove correct.

// CONST: const-index.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: negative element index through a slice parameter (the slice starts at the pointee, so elements before it are unreachable)
//--- const-index.c
static int prev(const int *p) { return p[-1]; }
int main(void) {
  int a[3] = {7, 8, 9};
  int b[3] = {1, 2, 3};
  return prev(&a[1]) + prev(&b[1]);
}

// A displacement of more than one element takes the same refusal.
// CONST2: const-index-2.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: negative element index through a slice parameter (the slice starts at the pointee, so elements before it are unreachable)
//--- const-index-2.c
static int prev2(const int *p) { return p[-2]; }
int main(void) {
  int a[4] = {7, 8, 9, 10};
  int b[4] = {1, 2, 3, 4};
  return prev2(&a[2]) + prev2(&b[2]);
}

// The pointer-arithmetic spelling `*(p - 1)` folds to the same cursor and
// must take the same refusal -- a fence that only saw the bracket form
// would leave the identical miscompile reachable one token away.
// DEREF: deref-minus.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: negative element index through a slice parameter (the slice starts at the pointee, so elements before it are unreachable)
//--- deref-minus.c
static int prev(const int *p) { return *(p - 1); }
int main(void) {
  int a[3] = {7, 8, 9};
  int b[3] = {1, 2, 3};
  return prev(&a[1]) + prev(&b[1]);
}

// Three storage bases keep the class on the same slice lowering.
// THREE: three-objects.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: negative element index through a slice parameter (the slice starts at the pointee, so elements before it are unreachable)
//--- three-objects.c
static int prev(const int *p) { return p[-1]; }
int main(void) {
  int a[3] = {7, 8, 9};
  int b[3] = {1, 2, 3};
  int c[3] = {4, 5, 6};
  return prev(&a[1]) + prev(&b[1]) + prev(&c[1]);
}

// The WRITE side of the same place: silently storing through a wrapped
// index would corrupt memory rather than merely read the wrong element,
// so the assign context is refused at the place, not at the load.
// WRITE: write-back.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: negative element index through a slice parameter (the slice starts at the pointee, so elements before it are unreachable)
//--- write-back.c
static void bump_prev(int *p) { p[-1] = p[-1] + 1; }
int main(void) {
  int a[3] = {7, 8, 9};
  int b[3] = {1, 2, 3};
  bump_prev(&a[1]);
  bump_prev(&b[1]);
  return a[0] + b[0];
}

// A cursor LOCAL derived from the slice parameter carries the parameter's
// origin, so walking it below that origin is the same unrepresentable
// access and takes the same refusal.
// CURSOR: cursor-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: negative element index through a slice parameter (the slice starts at the pointee, so elements before it are unreachable)
//--- cursor-local.c
static int prev(const int *p) {
  const int *q = p - 1;
  return *q;
}
int main(void) {
  int a[3] = {7, 8, 9};
  int b[3] = {1, 2, 3};
  return prev(&a[1]) + prev(&b[1]);
}

// The parameter's OWN cursor walked below its origin. This one writes the
// cursor cell twice (the entry `= 0` and the decrement), so it only folds
// through the reaching-store rule -- and it is not an academic case: it is
// the shape that still emitted a literal `p[(-1i64) as usize]` after the
// first, single-store-only version of the fence.
// DECR: decrement.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: negative element index through a slice parameter (the slice starts at the pointee, so elements before it are unreachable)
//--- decrement.c
static int back(const int *p) {
  p--;
  return *p;
}
int main(void) {
  int a[3] = {7, 8, 9};
  int b[3] = {1, 2, 3};
  return back(&a[1]) + back(&b[1]);
}
