// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/local-array.c 2>&1 | FileCheck %s --check-prefix=LOCALARRAY
// RUN: not emitrust-import-c %t/nonconst.c 2>&1 | FileCheck %s --check-prefix=NONCONST
// RUN: not emitrust-import-c %t/literal.c 2>&1 | FileCheck %s --check-prefix=LITERAL

// FR-153 fence R2: TWO cursor arguments in one call that walk the SAME
// region, where the callee's region slot is MUTABLE, is a LOCATED
// rejection.
//
// A cursor parameter's region base borrows mutably when the callee's body
// forwards `*p` on to a mutable parameter. Two such parameters fed from
// one region would hand out two mutable borrows of one place, which is
// `error[E0499]: cannot borrow ... as mutable more than once at a time`
// in the emitted crate — an emitted crate that does not compile, found
// only after cargo ran. Rejection is a feature: this moves the failure to
// a located diagnostic at the call, which is strictly better than a
// downstream rustc error, and it can never silently emit wrong code.
//
// The region key is the cursor local's proven base — the caller's VarDecl
// for an ordinary region, the literal's backing place for a
// literal-rooted one — so all three roots below are covered. Two cursor
// arguments over DIFFERENT regions, and two over the same region when the
// callee's slot is SHARED, are both still accepted; those are pinned
// positively in pointers-cursor-region-demand.c.

//--- local-array.c
// Two `const char **` cursors over one local array. `peek`'s parameter is
// a `const char *`, which still maps to a MUTABLE byte slice, so both
// cursor parameters' region bases are mutable.
int printf(const char *, ...);
int peek(const char *s) { return (int)s[0]; }
void adv2(const char **a, const char **b) {
  int x = peek(*a) + peek(*b);
  *a = *a + 1;
  *b = *b + 1;
  printf("%d\n", x);
}
int main(void) {
  char buf[5] = {'a', 'b', 'c', 'd', 0};
  const char *p = buf;
  const char *q = buf + 2;
  adv2(&p, &q);
  return 0;
}
// LOCALARRAY: local-array.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: two cursor arguments walk the same region and the callee needs a mutable region borrow

//--- nonconst.c
// The same shape spelled with non-const `char **`: the mutable region
// slot is unambiguous here, not an artifact of const not being honored.
int printf(const char *, ...);
int peek2(char *s) { return (int)s[0]; }
void adv3(char **a, char **b) {
  int x = peek2(*a) + peek2(*b);
  *a = *a + 1;
  *b = *b + 1;
  printf("%d\n", x);
}
int main(void) {
  char buf[5] = {'a', 'b', 'c', 'd', 0};
  char *p = buf;
  char *q = buf + 2;
  adv3(&p, &q);
  return 0;
}
// NONCONST: nonconst.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: two cursor arguments walk the same region and the callee needs a mutable region borrow

//--- literal.c
// Both cursors root in ONE string literal's backing. A literal-rooted
// region has no VarDecl base at all, so the aliasing key is the backing
// place itself; without it this pair would slip through the VarDecl-keyed
// half of the check.
int printf(const char *, ...);
int peek(const char *s) { return (int)s[0]; }
void adv2(const char **a, const char **b) {
  int x = peek(*a) + peek(*b);
  *a = *a + 1;
  *b = *b + 1;
  printf("%d\n", x);
}
int main(void) {
  const char *p = "abcd";
  const char *q = p + 2;
  adv2(&p, &q);
  return 0;
}
// LITERAL: literal.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: two cursor arguments walk the same region and the callee needs a mutable region borrow
