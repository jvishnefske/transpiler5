// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/conditional-write.c 2>&1 | FileCheck %s --check-prefix=COND
// RUN: not emitrust-import-c %t/disagreeing-roots.c 2>&1 | FileCheck %s --check-prefix=DISAGREE
// RUN: not emitrust-import-c %t/unrooted-write.c 2>&1 | FileCheck %s --check-prefix=UNROOTED
// RUN: not emitrust-import-c %t/caller-base-mismatch.c 2>&1 | FileCheck %s --check-prefix=MISMATCH
// RUN: not emitrust-import-c %t/prototype-only.c 2>&1 | FileCheck %s --check-prefix=PROTO

// Boundaries of the paired out-cursor shape (C99-43 slice 1b,
// pointers-cursor-param-paired.c). Each case pins the exact wording,
// because the wording IS the ledger-tag split: the substring table in
// RejectionLedger.cpp (and its Python twin in run_realworld.py) routes
// the shape failures below to the ptr-to-ptr-shape-escape family,
// ahead of the generic ptr-to-ptr row. The historical cases (1)
// `*endp = NULL` and (2) `*out = <one global>` moved to the ADMITTED
// side under C99-43 front C1 (single-global-or-NULL Option-cell
// mapping, pointers-cursor-param-global.c); the residual multi-global /
// outside-grammar rejections and their narrowed "global address"
// wordings are pinned in pointers-cursor-param-global-invalid.c. A
// prototype without a visible definition never reaches planning
// and keeps the historical generic wording (recorded in design.md:
// class-scope C++ methods reach mapParamType for DEFINED functions too,
// so the no-def wording split was measured off).

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
