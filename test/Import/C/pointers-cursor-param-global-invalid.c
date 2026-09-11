// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/two-globals-ternary.c 2>&1 | FileCheck %s --check-prefix=TWOTERN
// RUN: not emitrust-import-c %t/two-globals-writes.c 2>&1 | FileCheck %s --check-prefix=TWOWRITE
// RUN: not emitrust-import-c %t/mixed-root.c 2>&1 | FileCheck %s --check-prefix=MIXED
// RUN: not emitrust-import-c %t/global-offset.c 2>&1 | FileCheck %s --check-prefix=OFFSET
// RUN: not emitrust-import-c %t/element-mismatch.c 2>&1 | FileCheck %s --check-prefix=ELEM
// RUN: not emitrust-import-c %t/guarded-write.c 2>&1 | FileCheck %s --check-prefix=GUARD
// RUN: not emitrust-import-c %t/passed-on.c 2>&1 | FileCheck %s --check-prefix=PASSED
// RUN: not emitrust-import-c %t/stored.c 2>&1 | FileCheck %s --check-prefix=STORED

// Boundaries of the C99-43 C1 single-global-or-NULL out-param shape
// (pointers-cursor-param-global.c). Each case pins the exact wording,
// because the wording IS the ledger-tag split: the substring table in
// RejectionLedger.cpp (and its line-for-line Python twin in
// run_realworld.py) routes a "global address" wording to
// ptr-to-ptr-global-target — narrowed by C1 to the residual
// MULTI-global / outside-grammar cases (the FR-62 actor-decomposition
// front), single-global-or-NULL now being admitted — and the shape
// escapes to ptr-to-ptr-shape-escape. FR-241 moved that deliberately
// broad needle BELOW the returned-pointer rows: at its old position it
// also swallowed ImportCTypes.cpp's two returned-global-address
// refusals, so a reader ranking this cursor-parameter front was
// counting returned-pointer work in it. Any use of `p` outside `*p`
// (including the variant-B `if (p)` guard) stays an escape: the outer
// pointer has no representation under the `&mut Option<i64>` mapping.

// (1) One write, two distinct globals: `cond ? g1 : g2` needs a region
// selection the one-cell Option cursor cannot carry (Q2: synthesized
// region structs stay prohibited; multi-global state is the FR-62
// front).
//--- two-globals-ternary.c
unsigned a_flags[4];
unsigned b_flags[4];
void pick(int c, unsigned **out) {
  *out = c ? a_flags : b_flags;
}
int main(void) {
  unsigned *e;
  pick(1, &e);
  return 0;
}
// TWOTERN: two-globals-ternary.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cursor parameter written with more than one global address (single-global-or-NULL is the admitted C1 shape; multi-global state is the FR-62 front)

// (2) Two write sites naming two distinct globals: the same
// multi-global front, split across writes.
//--- two-globals-writes.c
unsigned a_flags[4];
unsigned b_flags[4];
void flip(int c, unsigned **out) {
  *out = a_flags;
  *out = b_flags;
}
int main(void) {
  unsigned *e;
  flip(1, &e);
  return 0;
}
// TWOWRITE: two-globals-writes.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cursor parameter written with more than one global address (single-global-or-NULL is the admitted C1 shape; multi-global state is the FR-62 front)

// (3) A global mixed with a non-global root in the RHS set: the cell's
// offsets are relative to ONE statically-known backing.
//--- mixed-root.c
unsigned g_flags[4];
void mix(int c, unsigned **out) {
  unsigned local[4];
  *out = c ? g_flags : local;
}
int main(void) {
  unsigned *e;
  mix(0, &e);
  return 0;
}
// MIXED: mixed-root.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cursor parameter written with a global address outside the single-global-or-NULL shape

// (4) A derived address (`g + 1`): only the WHOLE global (offset 0) is
// in the C1 grammar.
//--- global-offset.c
unsigned g_flags[4];
void inside(unsigned **out) {
  *out = g_flags + 1;
}
int main(void) {
  unsigned *e;
  inside(&e);
  return 0;
}
// OFFSET: global-offset.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cursor parameter written with a global address outside the single-global-or-NULL shape

// (5) Element-type mismatch between the parameter and the global's
// backing: the caller-side reads could not type-check against g.
//--- element-mismatch.c
int counter;
unsigned **hole;
void steal(unsigned **out) {
  *out = (unsigned *)&counter;
}
int main(void) {
  unsigned *e;
  steal(&e);
  return 0;
}
// ELEM: element-mismatch.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cursor parameter written with a global address outside the single-global-or-NULL shape

// (6) Variant B: the `if (p)` guard uses `p` outside `*p` and stays the
// historical shape escape (the outer pointer has no representation
// under the `&mut Option<i64>` mapping; the erased-guard lowering needs
// the all-sites proof recorded on FR-58's axis).
//--- guarded-write.c
unsigned g_flags[4];
void guarded(int c, unsigned **out) {
  if (out)
    *out = c ? g_flags : 0;
}
int main(void) {
  unsigned *e;
  guarded(1, &e);
  return 0;
}
// GUARD: guarded-write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape

// (7) Passing the outer pointer onward is the same escape.
//--- passed-on.c
unsigned g_flags[4];
void inner(unsigned **out) {
  *out = g_flags;
}
void outer(unsigned **out) {
  inner(out);
}
int main(void) {
  unsigned *e;
  outer(&e);
  return 0;
}
// PASSED: passed-on.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape

// (8) Storing the outer pointer is the same escape.
//--- stored.c
unsigned g_flags[4];
unsigned **kept;
void keep(unsigned **out) {
  kept = out;
  *out = g_flags;
}
int main(void) {
  unsigned *e;
  keep(&e);
  return 0;
}
// STORED: stored.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter escapes the cursor-parameter shape
