// REQUIRES: cargo
// FR-202 capability pin, runtime half. Teaching the FR-48 same-object
// check to look through the `&` of a pointer argument turns `a.m(&a)` into
// a located rejection; the cost of getting that rooting wrong is that the
// ordinary, non-aliasing `a.m(&b)` -- the commonest way C++ hands one
// object to another object's method -- stops importing, or worse, keeps
// importing but borrows the wrong place. test/Import/Cpp/
// cpp-method-self-alias-pointer.cpp pins the emitted operands; this file
// pins the ANSWERS, byte-diffed against the clang++ native, because the IR
// check cannot see a borrow that names the right place and reads the wrong
// value.
//
// Every value derives from argc and the binary is run twice (argc=1 and
// argc=2 via the `seed` argument), so no leg can be constant-folded into
// agreement.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_method_pointer_arg > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native seed > %t.native2.out
// RUN: %t.crate/target/release/cpp_method_pointer_arg seed > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out

extern "C" int printf(const char *, ...);

struct Inner {
  int v;
};

struct S {
  Inner inner;
  int a;
  void merge(const S *o) { a += o->a; }
  int peek(const S *o) const { return a * 10 + o->a; }
  void soak(const Inner *o) { a += o->v; }
  void pair(const Inner *p, const Inner *q) { a += p->v * 2 + q->v * 3; }
};

int main(int argc, char **) {
  S s;
  s.a = argc + 1;
  s.inner.v = argc * 7;
  S t;
  t.a = argc * 3 + 2;
  t.inner.v = argc + 100;

  // Two distinct receivers: the argument borrow names `t`, not `s`.
  s.merge(&t);
  printf("merge %d\n", s.a);

  // A const method's borrow of a different object, and the reverse
  // direction, so a rooting bug that confused receiver with argument shows
  // up as swapped operands rather than as a rejection.
  printf("peek %d %d\n", s.peek(&t), t.peek(&s));

  // The address of a MEMBER of the other object.
  s.soak(&t.inner);
  printf("soak %d\n", s.a);

  // Two pointer arguments, both distinct from each other and from the
  // receiver.
  Inner p;
  p.v = argc + 4;
  Inner q;
  q.v = argc + 9;
  s.pair(&p, &q);
  printf("pair %d\n", s.a);

  // The receiver's OWN member handed to a method of a DIFFERENT object:
  // `&s.inner` borrows `s`, and the receiver here is `t`, so the roots
  // differ and this must keep working.
  t.soak(&s.inner);
  printf("cross %d\n", t.a);
  return 0;
}
