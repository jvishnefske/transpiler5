// FR-122 located-rejection ledger for cross-TU MEMBER-SURFACE conflicts.
// The cross-TU record dedup key was FIELD-SHAPE-ONLY, so two records that
// resolve to the same emitted name with identical fields but DIFFERENT
// member semantics merged silently -- and the merged struct_def carried
// whichever TU's members were imported first. Measured at HEAD by
// byte-diff (both orders miscompile, with opposite polarities, in STRICT
// mode with zero diagnostics): droppy-TU-first gives the POD a phantom
// destructor at end of main; POD-first DELETES the legitimate dtor line.
// The load-bearing channel is the FR-53 idiomatic rename: `struct c` (POD)
// beside droppy `struct C` are DISTINCT C++ types with fully defined
// native behavior, so the miscompile cannot be dismissed as an
// ODR-violating input -- which is also why these legs run emitrust-cc
// (rename ON); emitrust-import-c keeps `@c`/`@C` distinct and never
// collides that pair.
//
// The fix appends `odr:<getODRHash()>;` to the dedup shape key for records
// carrying a member surface (user-declared dtor or any non-implicit
// method), so every silent channel becomes the EXISTING loud cross-TU
// wording, raised at the second-defining TU. REJECT, never rename or
// coexist: FR-108's measured NO on order-dependent disambiguators binds
// here too (record symbols are recomputed AST-pure in the item graph).
// A drop-presence BIT was measured and rejected as designed: it cannot
// see the METHOD-BODY and DTOR-BODY theft channels pinned below, which
// is why the key hashes the whole member surface.
//
// Channels pinned, each in both TU orders (the defect was order-sensitive,
// the diagnostic must not be):
//  * dtor presence: POD `struct c` vs droppy `struct C`;
//  * method-body theft: same fields, same-named method, different bodies;
//  * dtor-body theft: both sides droppy, different dtor bodies;
//  * same-spelling method-body divergence through emitrust-import-c
//    (rename OFF), so the sharpened key is pinned on both tools.
// RUN: split-file %s %t
// RUN: not emitrust-cc --emit=rust %t/droppy.cpp %t/pod.cpp 2>&1 | FileCheck %s --check-prefix=DTOR-DF
// RUN: not emitrust-cc --emit=rust %t/pod.cpp %t/droppy.cpp 2>&1 | FileCheck %s --check-prefix=DTOR-PF
// RUN: not emitrust-cc --emit=rust %t/method-lo.cpp %t/method-up.cpp 2>&1 | FileCheck %s --check-prefix=MBODY
// RUN: not emitrust-cc --emit=rust %t/method-up.cpp %t/method-lo.cpp 2>&1 | FileCheck %s --check-prefix=MBODY-REV
// RUN: not emitrust-cc --emit=rust %t/dtor-lo.cpp %t/dtor-up.cpp 2>&1 | FileCheck %s --check-prefix=DBODY
// RUN: not emitrust-cc --emit=rust %t/dtor-up.cpp %t/dtor-lo.cpp 2>&1 | FileCheck %s --check-prefix=DBODY-REV
// RUN: not emitrust-import-c %t/same-a.cpp %t/same-b.cpp 2>&1 | FileCheck %s --check-prefix=SAME

// The FR-42/FR-52 recovery half: under --recover the conflicting record is
// DROPPED (never merged), the function naming it takes a loud
// `unimplemented!()` stub, and the drop tabulates under the FR-122 tag
// `struct-shape-conflict` (previously the catch-all [other]), mirrored in
// lib/ImportC/RejectionLedger.cpp and test/RealWorld/run_realworld.py.
// The `--implicit-check-not` on the POD body's `+ 5` proves the dropped
// TU's member semantics never reach the emitted Rust under any mode.
// RUN: emitrust-cc --recover --emit=rust %t/droppy.cpp %t/pod.cpp %t/rec-main.cpp | FileCheck %s --check-prefix=REC --implicit-check-not="+ 5i32"
// RUN: emitrust-cc --recover --emit=rust %t/droppy.cpp %t/pod.cpp %t/rec-main.cpp 2>&1 >/dev/null | FileCheck %s --check-prefix=RECDIAG

//--- pod.cpp
extern "C" int printf(const char *, ...);
struct c {
  int v;
};
int use_pod(int x) {
  struct c p;
  p.v = x + 5;
  printf("pod %d\n", p.v);
  return p.v;
}

//--- droppy.cpp
extern "C" int printf(const char *, ...);
struct C {
  int v;
  ~C() { printf("dtor C %d\n", v); }
};
int use_droppy(int x) {
  C d;
  d.v = x + 1;
  printf("droppy %d\n", d.v);
  return d.v;
}

// Droppy TU first: the POD's TU is the second definer and carries the
// located rejection. Before the fix this order gave the POD a phantom
// destructor (byte-diffed: `dtor C 6` inserted at end of main).
// DTOR-DF: pod.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of struct 'C' with a different shape in another translation unit

// POD first: same wording at the droppy TU. Before the fix this order
// DELETED the legitimate `dtor C 2` line from the program's output.
// DTOR-PF: droppy.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of struct 'C' with a different shape in another translation unit

//--- method-lo.cpp
extern "C" int printf(const char *, ...);
struct m {
  int v;
  int get() const { return v; }
};
int use_l(int x) {
  m a;
  a.v = x + 6;
  printf("L %d\n", a.get());
  return a.get();
}

//--- method-up.cpp
extern "C" int printf(const char *, ...);
struct M {
  int v;
  int get() const { return v + 100; }
};
int use_u(int x) {
  M a;
  a.v = x;
  printf("U %d\n", a.get());
  return a.get();
}

// Method-BODY theft, the channel a drop bit cannot see (measured before
// the fix: `L 7` -> `L 107`, `U 101` -> `U 1` depending on order).
// MBODY: method-up.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of struct 'M' with a different shape in another translation unit
// MBODY-REV: method-lo.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of struct 'M' with a different shape in another translation unit

//--- dtor-lo.cpp
extern "C" int printf(const char *, ...);
struct d {
  int v;
  ~d() { printf("bye lower %d\n", v); }
};
int use_dl(int x) {
  d a;
  a.v = x + 6;
  return a.v;
}

//--- dtor-up.cpp
extern "C" int printf(const char *, ...);
struct D {
  int v;
  ~D() { printf("bye UPPER %d\n", v); }
};
int use_du(int x) {
  D a;
  a.v = x;
  return a.v;
}

// Dtor-BODY theft: both sides droppy, so a has_drop bit agrees on both --
// only the body hash can tell them apart (measured before the fix:
// `bye UPPER 7` emitted where the native prints `bye lower 7`).
// DBODY: dtor-up.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of struct 'D' with a different shape in another translation unit
// DBODY-REV: dtor-lo.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of struct 'D' with a different shape in another translation unit

//--- same-a.cpp
struct M {
  int v;
  int get() const { return v; }
};
int use_s1(int x) {
  M a;
  a.v = x;
  return a.get();
}

//--- same-b.cpp
struct M {
  int v;
  int get() const { return v + 100; }
};
int use_s2(int x) {
  M a;
  a.v = x;
  return a.get();
}

// Same spelling, divergent method bodies, rename OFF: an IFNDR input, but
// "silently pick one body" is still a miscompile against either TU's
// intent, so import-c rejects it under the same wording.
// SAME: same-b.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of struct 'M' with a different shape in another translation unit

//--- rec-main.cpp
extern "C" int printf(const char *, ...);
int use_pod(int);
int use_droppy(int);
int main(int argc, char **) {
  int a = use_pod(argc);
  int b = use_droppy(argc);
  printf("sums %d %d\n", a, b);
  return 0;
}

// The surviving droppy record keeps its Drop impl; the POD's user is a
// LOUD stub, never a silently re-bound body.
// REC: fn use_droppy
// REC: fn use_pod
// REC: unimplemented!("unsupported: struct 'C' was rejected, so a type naming it cannot be imported")
// REC: impl Drop for C

// The located warning at the second-defining TU, the FR-122 ledger tag on
// the drop, and the FR-52 cascade on the function that named the dropped
// type.
// RECDIAG: pod.cpp:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: conflicting definition of struct 'C' with a different shape in another translation unit (recovered: item dropped)
// FR-115: the ledger spelling is the report's JOIN KEY, so the dropped
// record prints under its graph node key `C` (`recordRustName`, UpperCamel
// under the idiomatic rename), not its lowercase C spelling `c`.
// RECDIAG: dropped 'C' [struct-shape-conflict] unsupported: conflicting definition of struct 'C' with a different shape in another translation unit
// RECDIAG: stubbed 'use_pod' [rejected-type-cascade]
// RECDIAG-NOT: error:
