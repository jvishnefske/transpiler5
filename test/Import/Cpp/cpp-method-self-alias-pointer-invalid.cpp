// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/method-addr-of-self.cpp 2>&1 | FileCheck %s --check-prefix=METHOD
// RUN: not emitrust-import-c %t/const-method-addr-of-self.cpp 2>&1 | FileCheck %s --check-prefix=CONSTM
// RUN: not emitrust-import-c %t/member-addr-of-self.cpp 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: not emitrust-import-c %t/call-operator-addr-of-self.cpp 2>&1 | FileCheck %s --check-prefix=OPER
// RUN: not emitrust-import-c %t/this-addr-of-arg.cpp 2>&1 | FileCheck %s --check-prefix=THISARG

// FR-202 located-rejection ledger: a method call whose POINTER argument
// takes the address of the receiver.
//
// The invariant. FR-48 already rejects `a.m(a)` -- one object borrowed
// twice, once as the receiver and once as a reference argument -- because
// emitting it would produce a crate rustc refuses. A `T *` parameter maps
// to the SAME `&mut T` a `T &` parameter does, so `a.m(&a)` is the
// identical two-borrow shape spelled with an address-of. It was NOT
// rejected: the check rooted its argument with `placeExprRoot`, whose walk
// peels members and subscripts but bottoms out on a `UnaryOperator`, so
// `&a` reported "no known root" -- which that code reads as "cannot
// alias". The crate emitted two `emitrust.addr_of mut` of one place and
// cargo failed with E0499. `emitrust-cc` exited 0 the whole time, which is
// exactly why this survived: compile success is not the oracle.
//
// These stay REJECTED rather than being made to work. Splitting one
// object's borrow into two disjoint ones is an analysis this wave does not
// have, and the safe direction for an unsupported borrow shape is a
// LOCATED refusal, never a crate that fails downstream in rustc's words
// instead of ours. The non-aliasing spellings that must keep working are
// pinned in cpp-method-self-alias-pointer.cpp and, at runtime, in
// test/EndToEnd/cpp-method-pointer-arg.cpp.
//
// Every case reuses FR-48's existing wording verbatim ("aliasing mutable
// reference argument and method receiver"); the two spellings are one
// rule, so they must not drift into two messages. The line:col of each
// diagnostic is pinned exactly -- an aliasing rejection that loses its
// location is as bad as no rejection, since the user cannot find the call.

//--- method-addr-of-self.cpp
// The base shape: a non-const method whose pointer argument is the
// receiver's own address. The receiver borrow is `&mut s` (the method is
// non-const) and the argument borrow is `&mut s` too, so both are unique.
// METHOD: method-addr-of-self.cpp:13:3: error: unsupported: aliasing mutable reference argument and method receiver
struct S {
  int a;
  void merge(const S *o) { a += o->a; }
};

int use(void) {
  S s;
  s.a = 1;
  s.merge(&s);
  return s.a;
}

//--- const-method-addr-of-self.cpp
// A CONST method still collides, because the pointer parameter's mapped
// borrow is unique: `const S *` maps to `&mut S` today (the pointee's
// constness is not carried into the borrow's mutability), so the argument
// alone supplies the unique borrow the rule needs. If a later wave maps a
// pointer-to-const to `&S`, this case becomes an ACCEPT and moves to the
// companion file -- the pin exists so that move is deliberate rather than
// accidental.
// CONSTM: const-method-addr-of-self.cpp:17:10: error: unsupported: aliasing mutable reference argument and method receiver
struct S {
  int a;
  int peek(const S *o) const { return a + o->a; }
};

int use(void) {
  S s;
  s.a = 1;
  return s.peek(&s);
}

//--- member-addr-of-self.cpp
// The address of a MEMBER of the receiver is still a borrow of the
// receiver: `&s.inner` and `&mut s` overlap, and the place walk under the
// address-of reaches the same root `s`. Rejecting is exact here, not
// merely conservative -- rustc refuses the emitted form for the same
// reason.
// MEMBER: member-addr-of-self.cpp:21:3: error: unsupported: aliasing mutable reference argument and method receiver
struct Inner {
  int v;
};

struct S {
  Inner inner;
  int a;
  void soak(const Inner *o) { a += o->v; }
};

int use(void) {
  S s;
  s.a = 1;
  s.inner.v = 2;
  s.soak(&s.inner);
  return s.a;
}

//--- call-operator-addr-of-self.cpp
// The same rule on the MEMBER-OPERATOR call path, which carries its own
// copy of the check: `s(&s)` is `s.operator()(&s)`. The call operator is
// used rather than `+=` because a member `operator+=` with a POINTER
// parameter is omitted at class-import time for an unrelated reason ("call
// to overloaded operator 'operator+=' omitted from class"), so it never
// reaches this check at all.
// OPER: call-operator-addr-of-self.cpp:19:10: error: unsupported: aliasing mutable reference argument and method receiver
struct S {
  int a;
  int operator()(const S *o) {
    a += o->a;
    return a;
  }
};

int use(void) {
  S s;
  s.a = 1;
  return s(&s);
}

//--- this-addr-of-arg.cpp
// The receiver spelled implicitly: `merge(&*this)` inside a method. No
// `VarDecl` can name the receiver, so the `this`-rooted arm of the same
// rule answers -- and it too has to look through the address-of. FR-112
// contains the failure to the enclosing method, so the located rejection
// is a WARNING naming the omitted method plus an error at its use, exactly
// as the reference spelling's `merge(*this)` case does in
// cpp-implicit-this-invalid.cpp.
// THISARG: this-addr-of-arg.cpp:13:23: warning: unsupported: aliasing mutable reference argument and method receiver (omitted: method 'self_merge' of class 'Node')
// THISARG: this-addr-of-arg.cpp:19:3: error: unsupported: call to unimported method 'Node_self_merge'
struct Node {
  int v;
  void merge(const Node *o) { v += o->v; }
  void self_merge() { merge(&*this); }
};

int use(void) {
  Node n;
  n.v = 1;
  n.self_merge();
  return n.v;
}
