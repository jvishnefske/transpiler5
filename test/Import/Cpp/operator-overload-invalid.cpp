// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/shl.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=SHL --implicit-check-not="op_shl"
// RUN: not emitrust-import-c %t/optmpl.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=OPTMPL --implicit-check-not="op_eq"
// RUN: not emitrust-import-c %t/refret.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=REFRET --implicit-check-not="S_op_index"
// RUN: not emitrust-import-c %t/postinc.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=POSTINC --implicit-check-not="S_op"
// RUN: not emitrust-import-c %t/chain.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CHAIN
// RUN: not emitrust-import-c %t/collide-mem.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=COLLIDEMEM
// RUN: not emitrust-import-c %t/collide-free.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=COLLIDEFREE
// RUN: not emitrust-import-c %t/collide-free-rev.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=COLLIDEREV
// RUN: not emitrust-import-c %t/const-pair.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=CONSTPAIR
// RUN: not emitrust-import-c %t/friend-call.cpp 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FRIENDCALL

// W2.25: the FENCES around the admitted operator subset. Admission is
// TABLE-DRIVEN (overloadedOperatorSymbolBaseName in CSymbolNaming.h), so
// every kind outside the table keeps its historical located rejection, and
// every SHAPE failure inside an admitted kind rides the FR-112 omission
// channel with its own per-construct diagnostic — nothing may silently
// emit wrong code, and no synthesized spelling may silently fuse with a
// user identifier that happens to spell the same. Each section pins one
// fence:
//
//  - shl/optmpl: non-admitted KINDS (`operator<<` on a user class; an
//    operator TEMPLATE, whose W2.15 suffix interplay was never spiked)
//    keep FR-119's def-site rejection, and the --implicit-check-not pins
//    that no synthesized symbol leaks.
//  - refret: a reference-returning member operator of an ADMITTED kind
//    (`int &operator[]`) is omitted by `importFunction`'s existing
//    reference-return fence (a located WARNING naming the operator), and
//    the use dies on the lvalue machinery — never a wrong-body call.
//  - postinc: `operator++` stays outside the table (its canonical
//    `S r = *this` body has no image), so the spelled use keeps the
//    FR-112 omitted-member wording.
//  - chain: chained MEMBER operators (`a + b + c`) need a method receiver
//    on a call-result temporary, which the place machinery refuses —
//    located, pre-existing wording, same as an identifier method chain.
//  - collide-mem / collide-free(-rev): the synthesized spelling vs a user
//    function literally named `op_eq`, member and free, BOTH declaration
//    orders. Neither direction may merge into one overload set — members
//    reject through the W2.25 sibling walk (both colliders omitted, uses
//    located), free functions through the FR-125 qualified-owner guard.
//  - const-pair: a const/non-const operator pair with identical
//    parameters composes ONE suffixed symbol (the FR-114 residual); the
//    second overload is omitted with the honest overload-set wording and
//    a call resolving to the OMITTED side is caught loudly by the
//    `emitrust.addr_of` mut-marker verifier — measured identical to the
//    identifier-method twin (`get`/`get const`), never a silent
//    wrong-body call.
//  - friend-call: a friend operator defined INLINE in the class is still
//    the FR-123 silent-omission channel (its DEF leaves no trace — pinned
//    as-is by free-operator-invalid.cpp's FRIEND section), but a CALL to
//    one of an admitted kind now names the missing synthesized symbol
//    instead of the old bare "unsupported callee" — the channel stays a
//    located error in every mode, only more rankable.

//--- shl.cpp
// SHL: shl.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: overloaded operator
struct A {
  int v;
};
A operator<<(A a, int b) { A r; r.v = a.v << b; return r; }

//--- optmpl.cpp
// OPTMPL: optmpl.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: overloaded operator
struct B {
  int v;
};
template <typename T> bool operator==(const B &a, T b) { return a.v == b; }
int use(int n) {
  B a;
  a.v = n;
  return (a == 3) ? 1 : 0;
}

//--- refret.cpp
// REFRET: refret.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: reference return types are not yet supported (omitted: method 'operator[]' of class 'S')
// REFRET: refret.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported assignable expression: CXXOperatorCallExpr
struct S {
  int a[4];
  int &operator[](int i) { return a[i]; }
};
int use(int n) {
  S s;
  s.a[0] = n;
  return s[0];
}

//--- postinc.cpp
// POSTINC: postinc.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to overloaded operator 'operator++' omitted from class 'S'
struct S {
  int v;
  S operator++(int) {
    S r;
    r.v = v;
    v = v + 1;
    return r;
  }
};
int use(int n) {
  S s;
  s.v = n;
  S t = s++;
  return t.v;
}

//--- chain.cpp
// CHAIN: chain.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported assignable expression: CXXOperatorCallExpr
struct S {
  int v;
  S operator+(const S &o) const {
    S r;
    r.v = v + o.v;
    return r;
  }
};
int use(int n) {
  S a, b, c;
  a.v = n;
  b.v = 1;
  c.v = 2;
  S d = a + b + c;
  return d.v;
}

//--- collide-mem.cpp
// BOTH colliders are omitted (each sees the other as its sibling), so the
// class still imports and the use is a located rejection — never a merged
// overload set, never a wrong-body call.
// COLLIDEMEM: collide-mem.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: method 'op_eq' emits as 'S_op_eq', which collides with 'operator==' (the synthesized operator spelling folds both onto one symbol) (omitted: method 'op_eq' of class 'S')
// COLLIDEMEM: collide-mem.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: method 'operator==' emits as 'S_op_eq', which collides with 'op_eq' (the synthesized operator spelling folds both onto one symbol) (omitted: method 'operator==' of class 'S')
// COLLIDEMEM: collide-mem.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to unimported method 'S_op_eq'
struct S {
  int v;
  bool op_eq(const S &o) const { return v == o.v; }
  bool operator==(const S &o) const { return v == o.v; }
};
int use(int n) {
  S a, b;
  a.v = n;
  b.v = n;
  return a.op_eq(b) ? 1 : 0;
}

//--- collide-free.cpp
// The user `op_eq` declares FIRST and owns the spelling; the operator's
// import rejects at its own declaration through the FR-125 guard.
// COLLIDEFREE: collide-free.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function 'operator==' emits as 'op_eq', which collides with 'op_eq' (the idiomatic rename folds both spellings onto one symbol)
struct S {
  int v;
};
bool op_eq(const S &a, const S &b) { return a.v == b.v; }
bool operator==(const S &a, const S &b) { return a.v == b.v; }
int use(int n) {
  S a, b;
  a.v = n;
  b.v = n;
  return op_eq(a, b) ? 1 : 0;
}

//--- collide-free-rev.cpp
// Reversed order: the operator declares FIRST and claims `op_eq`; the
// user function's import rejects at ITS declaration. Symmetric, so no
// declaration order can slip a silent fusion through.
// COLLIDEREV: collide-free-rev.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function 'op_eq' emits as 'op_eq', which collides with 'operator==' (the idiomatic rename folds both spellings onto one symbol)
struct S {
  int v;
};
bool operator==(const S &a, const S &b) { return a.v == b.v; }
bool op_eq(const S &a, const S &b) { return a.v == b.v; }
int use(int n) {
  S a, b;
  a.v = n;
  b.v = n;
  return op_eq(a, b) ? 1 : 0;
}

//--- const-pair.cpp
// CONSTPAIR: const-pair.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: C++ overload set for 'operator()' maps two overloads onto one emitted symbol 'S_op_call_i' (parameter types not distinguishable in the overload suffix) (omitted: method 'operator()' of class 'S')
// CONSTPAIR: const-pair.cpp:{{[0-9]+}}:{{[0-9]+}}: error: 'emitrust.addr_of' op result is a !emitrust.mut_ref but the mut marker is absent
struct S {
  int v;
  int operator()(int i) { return v + i; }
  int operator()(int i) const { return v - i; }
};
int use(const S s) { return s(1); }

//--- friend-call.cpp
// FRIENDCALL: friend-call.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to unimported function 'op_add'
struct B {
  int v;
  friend int operator+(B a, int b) { return a.v + b; }
};
int use(int n) {
  B b;
  b.v = n;
  return b + 5;
}
