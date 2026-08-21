// REQUIRES: cargo
// FR-102 struct-pointer components in fn-ptr types, differential
// end-to-end test. `cargo build` success is compile-only and cannot see a
// miscompile, so the oracle here is a BYTE-DIFF of the emitted crate's
// stdout against the clang-built native, in THREE argc branches: every
// seed, every branch selection and every callback argument derives from
// argc, so constant folding cannot hide a wrong dispatch behind an
// identical constant.
//
// The two shapes under test:
//  - a callback table over an UNRELATED complete struct
//    (`int (*)(struct payload *, int)`), assigned from an argc-dependent
//    branch and dispatched through the struct MEMBER with a `&mut`
//    receiver — this is what `Option<fn(&mut Payload, i32) -> i32>` plus
//    `.expect("null function pointer")` has to execute correctly;
//  - a SELF-REFERENTIAL callback (`struct node { int (*visit)(struct node
//    *, int); }`) invoked repeatedly through the very object that holds
//    the pointer and MUTATING it each time. This is the borrow-checker
//    question the FR-102 spike answered by hand: the fn pointer is a Copy
//    value read out of the member before the `&mut` receiver is taken, so
//    the self-alias is accepted and each call must observe the previous
//    call's mutation in order.
//
// main returns 0 and reports everything on stdout, so lit's exit-code
// checking covers all three runs and diff covers the observable behavior.
// The program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native0.out
// RUN: %t.crate/target/release/fnptr_struct_components > %t.rust0.out
// RUN: diff %t.native0.out %t.rust0.out
// RUN: %t.native one > %t.native1.out
// RUN: %t.crate/target/release/fnptr_struct_components one > %t.rust1.out
// RUN: diff %t.native1.out %t.rust1.out
// RUN: %t.native one two three > %t.native3.out
// RUN: %t.crate/target/release/fnptr_struct_components one two three > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

#include <stdio.h>

struct payload {
  int lo;
  int hi;
};

struct ops {
  int (*apply)(struct payload *p, int n);
  int tag;
};

struct node {
  int val;
  int (*visit)(struct node *n, int depth);
};

static int add_bounds(struct payload *p, int n) {
  p->lo += n;
  p->hi -= n;
  return p->lo + p->hi;
}

static int scale_bounds(struct payload *p, int n) {
  p->lo *= n;
  p->hi *= n;
  return p->lo - p->hi;
}

static int visit_double(struct node *n, int depth) {
  n->val = n->val * 2 + depth;
  return n->val;
}

static int visit_shift(struct node *n, int depth) {
  n->val = n->val - 3 * depth + 1;
  return n->val;
}

int main(int argc, char **argv) {
  struct payload pay;
  pay.lo = 3 + argc;
  pay.hi = 40 + argc * 2;

  struct ops sel;
  if (argc > 2) {
    sel.apply = scale_bounds;
    sel.tag = 9;
  } else if (argc > 1) {
    sel.apply = add_bounds;
    sel.tag = 8;
  } else {
    sel.apply = scale_bounds;
    sel.tag = 7;
  }
  int r = sel.apply(&pay, argc + 2) + sel.tag;
  printf("r=%d tag=%d lo=%d hi=%d\n", r, sel.tag, pay.lo, pay.hi);

  // Re-dispatch the same member after rebinding it, so the branch is not
  // hoistable to a single constant callee.
  sel.apply = add_bounds;
  int r2 = sel.apply(&pay, argc);
  printf("r2=%d lo=%d hi=%d\n", r2, pay.lo, pay.hi);

  // Self-referential member, called repeatedly through its own holder.
  struct node nd;
  nd.val = argc * 5;
  if (argc > 1)
    nd.visit = visit_shift;
  else
    nd.visit = visit_double;
  int a = nd.visit(&nd, argc);
  int b = nd.visit(&nd, a);
  int c = nd.visit(&nd, b + argc);
  printf("a=%d b=%d c=%d val=%d\n", a, b, c, nd.val);
  return 0;
}
