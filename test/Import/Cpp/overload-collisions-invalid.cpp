// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/free-i64.cpp 2>&1 | FileCheck %s --check-prefix=FREEI64
// RUN: not emitrust-import-c %t/free-fnptr.cpp 2>&1 | FileCheck %s --check-prefix=FREEFNPTR
// RUN: not emitrust-import-c %t/member-int-long.cpp 2>&1 | FileCheck %s --check-prefix=MEMINT
// RUN: not emitrust-import-c %t/ctor-i64.cpp 2>&1 | FileCheck %s --check-prefix=CTORI64
// RUN: not emitrust-import-c %t/member-alias.cpp 2>&1 | FileCheck %s --check-prefix=MEMALIAS

// FR-114 located-rejection ledger for the overload shapes that STILL
// collide after the suffix widening — and for the HONEST wording that
// replaced the old one. Before FR-114, every same-TU overload collision
// was blamed on a phantom second translation unit ("conflicting
// definition of 'g' (already defined in another translation unit)") —
// wrong on both counts: one TU, and a genuine C++ overload set. The new
// wording names the real cause and the real limit (the suffix table),
// while the cross-TU wording survives untouched for genuine cross-TU
// duplicates (a same-named lookup of size 1 — see
// overload-cross-tu-asymmetric.cpp and the C multi-TU tests).
//
// The shapes pinned here are colliding BY DESIGN, each for a frozen or
// measured reason:
//
// * free-i64: `long` and `long long` both code `i64` — one Rust type,
//   one code, faithfully. Same class as double/long double (both `d`).
// * free-fnptr: two function-pointer parameters both code `px` (the
//   pointer arm nests its pointee, and a function prototype has no code
//   of its own — the same `x` it has under W2.15's template table).
// * member-int-long: the MEMBER integer code `i` is FROZEN by W2.2's
//   byte-for-byte CHECK pins (Counter_ctor_i, Counter_get_i, C_ctor_i), so
//   int/long and int/unsigned stay collided on the member path even
//   though the free path now splits int/long as _i32/_i64. A member
//   collision is downgraded to warning + omission by the class importer
//   (ImportCAggregates), so the pins here are the WARNING wrapper plus
//   the follow-on located error when the omitted overload is called.
// * ctor-i64: same frozen `i` on the constructor path (both code `_i`), and
//   a CONSTRUCTOR collision is a hard error (no omission downgrade).
// * member-alias: member codes concatenate WITHOUT separators, so
//   multi-char record codes can alias across different overloads —
//   `h(A, Bi)` and `h(Ab, I)` both compose the code string `abi`. FR-114
//   keeps the separator-free member scheme (nothing pinned moves) and
//   documents THIS collision guard as the backstop: the aliased pair is
//   never silently merged, it lands here, located, with the honest
//   wording.
//
// Every wording below interpolates the COMPUTED symbol (raw importer
// spelling, e.g. `M_h_i`, not the idiomatic-renamed `m_h_i`).

//--- free-i64.cpp
long g(long x) { return x + 1; }
// FREEI64: free-i64.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: C++ overload set for 'g' maps two overloads onto one emitted symbol 'g_i64' (parameter types not distinguishable in the overload suffix)
long long g(long long x) { return x + 2; }

//--- free-fnptr.cpp
void cb_i(int) {}
void cb_d(double) {}
int g(void (*f)(int)) { f(1); return 1; }
// FREEFNPTR: free-fnptr.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: C++ overload set for 'g' maps two overloads onto one emitted symbol 'g_px' (parameter types not distinguishable in the overload suffix)
int g(void (*f)(double)) { f(2.0); return 2; }

//--- member-int-long.cpp
class M {
public:
  int t;
  int h(int x) { return t + x; }
  long h(long x) { return t - x; }
};
class N {
public:
  int t;
  int h(int x) { return t + x; }
  unsigned h(unsigned x) { return x; }
};
int use(void) {
  M m;
  m.t = 1;
  return (int)m.h(2L);
}
// The class importer downgrades a member collision to warning+omission,
// so the class itself survives with the FIRST overload only ...
// MEMINT: member-int-long.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: C++ overload set for 'h' maps two overloads onto one emitted symbol 'M_h_i' (parameter types not distinguishable in the overload suffix) (omitted: method 'h' of class 'M')
// MEMINT: member-int-long.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: C++ overload set for 'h' maps two overloads onto one emitted symbol 'N_h_i' (parameter types not distinguishable in the overload suffix) (omitted: method 'h' of class 'N')
// ... and a call that resolves to the OMITTED overload fails loudly at
// the call site (never silently re-routed to the surviving one).
// MEMINT: member-int-long.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call argument type mismatch

//--- ctor-i64.cpp
class S {
public:
  long v;
  S(long x) : v(x) {}
  // CTORI64: ctor-i64.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: C++ overload set for 'S' maps two overloads onto one emitted symbol 'S_ctor_i' (parameter types not distinguishable in the overload suffix)
  S(long long x) : v((long)x) {}
};

//--- member-alias.cpp
struct A { int v; };
struct Bi { int v; };
struct Ab { int v; };
struct I { int v; };
class K {
public:
  int t;
  int h(A a, Bi b) { return t + a.v + b.v; }
  // MEMALIAS: member-alias.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: C++ overload set for 'h' maps two overloads onto one emitted symbol 'K_h_abi' (parameter types not distinguishable in the overload suffix) (omitted: method 'h' of class 'K')
  int h(Ab a, I b) { return t - a.v - b.v; }
};
int use(void) {
  K k;
  k.t = 1;
  A a;
  Bi b;
  a.v = 2;
  b.v = 3;
  Ab a2;
  I i2;
  a2.v = 4;
  i2.v = 5;
  return k.h(a2, i2);
}
// MEMALIAS: member-alias.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call argument type mismatch
