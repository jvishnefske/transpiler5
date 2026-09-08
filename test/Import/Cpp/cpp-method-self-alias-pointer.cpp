// RUN: emitrust-import-c %s | FileCheck %s

// FR-202 capability pin, the ACCEPT half of
// cpp-method-self-alias-pointer-invalid.cpp.
//
// Teaching the FR-48 same-object check to look through the `&` of a
// pointer argument makes previously-importing programs refuse (that is the
// point -- they were emitting crates rustc rejected). The risk it carries
// is OVER-rejection: a rooting walk that answered "same object" too often
// would silently amputate the ordinary `a.m(&b)` call, which is the single
// most common way C++ code hands one object to another's method. This file
// is the fence around that risk. Every call below borrows two DISTINCT
// objects and must keep emitting exactly the two `emitrust.addr_of mut`
// operands it emitted before FR-202 -- so the checks name the operand
// places, not just the call.
//
// The runtime half lives in test/EndToEnd/cpp-method-pointer-arg.cpp: this
// file proves the calls still IMPORT, that one proves they still compute
// the same answers as clang++.

struct Inner {
  int v;
};

struct S {
  Inner inner;
  int a;
  void merge(const S *o) { a += o->a; }
  int peek(const S *o) const { return a + o->a; }
  void soak(const Inner *o) { a += o->v; }
  void pair(const Inner *p, const Inner *q) { a += p->v + q->v; }
  void stir();
};

// Two distinct locals: the receiver's place and the argument's place are
// different `emitrust.variable`s, so nothing collides.
// CHECK-LABEL: func.func @use_two_locals
// CHECK: %[[S:.*]] = emitrust.variable named "s"
// CHECK: %[[T:.*]] = emitrust.variable named "t"
// CHECK: %[[RS:.*]] = emitrust.addr_of mut %[[S]]
// CHECK: %[[RT:.*]] = emitrust.addr_of mut %[[T]]
// CHECK: call @S_merge(%[[RS]], %[[RT]])
int use_two_locals(void) {
  S s;
  s.a = 1;
  S t;
  t.a = 2;
  s.merge(&t);
  return s.a;
}

// A CONST method with a pointer argument naming a different object: still
// two borrows, still distinct roots.
// CHECK-LABEL: func.func @use_const_method
// CHECK: %[[CS:.*]] = emitrust.variable named "s"
// CHECK: %[[CT:.*]] = emitrust.variable named "t"
// CHECK: %[[CRS:.*]] = emitrust.addr_of %[[CS]]
// CHECK: %[[CRT:.*]] = emitrust.addr_of mut %[[CT]]
// CHECK: call @S_peek(%[[CRS]], %[[CRT]])
int use_const_method(void) {
  S s;
  s.a = 1;
  S t;
  t.a = 2;
  return s.peek(&t);
}

// The address of a MEMBER of a different object. The place walk under the
// `&` reaches root `t`, not `s`, which is exactly the discrimination the
// new rooting has to get right -- rooting at "no known object" (the old
// behavior) and rooting at "the receiver" are both wrong here.
// CHECK-LABEL: func.func @use_other_member
// CHECK: %[[MS:.*]] = emitrust.variable named "s"
// CHECK: %[[MT:.*]] = emitrust.variable named "t"
// CHECK: %[[MRS:.*]] = emitrust.addr_of mut %[[MS]]
// CHECK: %[[MI:.*]] = emitrust.member %[[MT]]["inner"]
// CHECK: %[[MRI:.*]] = emitrust.addr_of mut %[[MI]]
// CHECK: call @S_soak(%[[MRS]], %[[MRI]])
int use_other_member(void) {
  S s;
  s.a = 1;
  S t;
  t.inner.v = 3;
  s.soak(&t.inner);
  return s.a;
}

// Two pointer arguments naming two distinct objects, neither of them the
// receiver: the argument-with-argument arm of the same rule must not fire
// either.
// CHECK-LABEL: func.func @use_two_args
// CHECK: call @S_pair
int use_two_args(void) {
  S s;
  s.a = 1;
  Inner p;
  p.v = 4;
  Inner q;
  q.v = 5;
  s.pair(&p, &q);
  return s.a;
}

// The receiver spelled implicitly, argument naming a LOCAL of the method:
// `this` and `tmp` are different objects, so the `this`-rooted arm of the
// rule stays quiet. (Forwarding a pointer PARAMETER on to a sibling is a
// separate, pre-existing rejection -- "the address of a scalar object
// cannot be passed as a slice parameter" -- and is not this rule's
// business.)
// CHECK-LABEL: func.func @S_stir
// CHECK: %[[TMP:.*]] = emitrust.variable named "tmp"
// CHECK: %[[RTMP:.*]] = emitrust.addr_of mut %[[TMP]]
// CHECK: call @S_soak(%{{.*}}, %[[RTMP]])
void S::stir() {
  Inner tmp;
  tmp.v = 2;
  soak(&tmp);
}

int use_stir(void) {
  S s;
  s.a = 1;
  s.stir();
  return s.a;
}
