// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/null-write.c 2>&1 | FileCheck %s --check-prefix=NULLW
// RUN: not emitrust-import-c %t/global-target.c 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: not emitrust-import-c %t/conditional-write.c 2>&1 | FileCheck %s --check-prefix=COND
// RUN: not emitrust-import-c %t/disagreeing-roots.c 2>&1 | FileCheck %s --check-prefix=DISAGREE
// RUN: not emitrust-import-c %t/unrooted-write.c 2>&1 | FileCheck %s --check-prefix=UNROOTED
// RUN: not emitrust-import-c %t/caller-base-mismatch.c 2>&1 | FileCheck %s --check-prefix=MISMATCH
// RUN: not emitrust-import-c %t/prototype-only.c 2>&1 | FileCheck %s --check-prefix=PROTO

// Boundaries of the paired out-cursor shape (C99-43 slice 1b,
// pointers-cursor-param-paired.c). Each case pins the exact wording,
// because the wording IS the ledger-tag split: the substring table in
// RejectionLedger.cpp (and its Python twin in run_realworld.py) routes
// "written with a null pointer" to ptr-to-ptr-null-write, "written with
// a global address" to ptr-to-ptr-global-target (the FR-62
// actor-decomposition front), and the remaining shape failures to the
// ptr-to-ptr-shape-escape family, all ahead of the generic ptr-to-ptr
// row. A prototype without a visible definition never reaches planning
// and keeps the historical generic wording (recorded in design.md:
// class-scope C++ methods reach mapParamType for DEFINED functions too,
// so the no-def wording split was measured off).

// (1) `*endp = NULL`: C's "no conversion" contract needs a NULL-flag
// threaded back to the caller — a second in-out state cell slice 1
// does not synthesize (Q2 reasoning).
//--- null-write.c
void reset(const char *s, const char **endp) {
  *endp = 0;
}
int main(void) {
  const char *t = "a";
  const char *e;
  reset(t, &e);
  return 0;
}
// NULLW: null-write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cursor parameter written with a null pointer

// (2) `*out = <global address>` (the check_cpu global-or-NULL family,
// u32** shape): multi-global state is the FR-62 front.
//--- global-target.c
unsigned err_flags[4];
void report(unsigned err, unsigned **out) {
  *out = err_flags;
}
int main(void) {
  unsigned *e;
  report(1u, &e);
  return 0;
}
// GLOBAL: global-target.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cursor parameter written with a global address (multi-global state is the FR-62 front)

// (3) A write nested in control flow: the caller-side writeback is
// unconditional, so admission demands the write execute on every path
// (syntactic: a top-level statement with no return/goto/label before
// it).
//--- conditional-write.c
void skip(const char *s, const char **endp) {
  if (s[0] == ' ')
    *endp = s + 1;
}
int main(void) {
  const char *t = " b";
  const char *e;
  skip(t, &e);
  return 0;
}
// COND: conditional-write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cursor parameter write must execute unconditionally before every return

// (4) Two write sites rooting in different co-parameters.
//--- disagreeing-roots.c
void pick(const char *a, const char *b, const char **endp) {
  *endp = a;
  *endp = b;
}
int main(void) {
  const char *e;
  pick("x", "y", &e);
  return 0;
}
// DISAGREE: disagreeing-roots.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cursor parameter write sites disagree on the source region

// (5) A single write whose RHS roots in no sibling slice parameter (a
// callee-local array is a dangling cursor the moment the frame dies).
//--- unrooted-write.c
void local_leak(const char *s, const char **endp) {
  char tmp[4];
  *endp = tmp;
}
int main(void) {
  const char *t = "a";
  const char *e;
  local_leak(t, &e);
  return 0;
}
// UNROOTED: unrooted-write.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: cursor parameter write does not root in a sibling slice parameter

// (6) Caller side: `&e` must walk the SAME region as the co-argument;
// binding e to a different object first makes the joined region
// multi-base, which the paired protocol's single-base writeback cannot
// take.
//--- caller-base-mismatch.c
long parse(const char *s, const char **endp) {
  *endp = s;
  return 0;
}
int main(void) {
  char one[2] = "a";
  char two[2] = "b";
  const char *e = two;
  return (int)parse(one, &e);
}
// MISMATCH: caller-base-mismatch.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a paired cursor argument must walk the co-argument's region

// (7) A prototype without a visible definition has no body to prove
// either cursor shape against; the generic rejection stands (FR-58's
// signature-starvation axis is the recorded cross-TU path).
//--- prototype-only.c
extern long parse_ext(const char *s, const char **endp);
int main(void) {
  const char *t = "a";
  const char *e;
  return (int)parse_ext(t, &e);
}
// PROTO: prototype-only.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer-to-pointer parameter
