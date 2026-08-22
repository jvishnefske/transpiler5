// REQUIRES: cargo
// FR-122: the cross-TU record dedup key folds in the ODR hash of the
// member surface, and this file holds the two EndToEnd ends of that fix.
//
// THE REJECTION LEGS (both TU orders): the idiomatic-rename channel --
// POD `struct c` in one TU, droppy `struct C` in another, DISTINCT C++
// types with fully defined native behavior -- used to byte-diff DIRTY in
// both orders with zero diagnostics: droppy-first inserted a phantom
// `dtor C 6` after `pod 6`; POD-first deleted the legitimate `dtor C 2`.
// Both crate builds exited 0, which is exactly why compile-clean evidence
// can never clear a codegen change. The full --emit=crate --build
// pipeline must now refuse this input LOUDLY in both orders -- these legs
// pin the rejection that replaced the measured silent miscompile.
//
// THE BYTE-DIFF LEG: the legitimate merge that must SURVIVE the sharpened
// key -- one droppy header (dtor + method) included by two TUs. Its
// stdout is diffed byte for byte against the clang++-built native; the
// dtor printf makes drop presence, drop order, and the merged method body
// all directly observable, and every value derives from argc so constant
// folding cannot pre-compute the answers. This leg passing also proves
// getODRHash is stable across the separate per-TU ASTContexts (a
// divergent hash would turn this merge into a rejection).
//
// Probe shape constraints (measured): droppy objects sit at FUNCTION
// scope (W2.17's scope gate rejects inner-block dtor objects before the
// dedup is ever reached) and each TU uses distinct function names (the
// duplicate-function rejection fires before any struct logic otherwise).
// RUN: split-file %s %t
// RUN: not emitrust-cc --emit=crate %t/droppy.cpp %t/pod.cpp %t/main.cpp -o %t.rej-df --crate-name odr_rej --build 2>&1 | FileCheck %s --check-prefix=REJ-DF
// RUN: not emitrust-cc --emit=crate %t/pod.cpp %t/droppy.cpp %t/main.cpp -o %t.rej-pf --crate-name odr_rej --build 2>&1 | FileCheck %s --check-prefix=REJ-PF
// RUN: emitrust-cc --emit=crate %t/keep-a.cpp %t/keep-b.cpp %t/keep-main.cpp -o %t.crate --crate-name odr_keep --build
// RUN: clang++ -std=c++17 %t/keep-a.cpp %t/keep-b.cpp %t/keep-main.cpp -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/odr_keep > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

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

//--- main.cpp
extern "C" int printf(const char *, ...);
int use_pod(int);
int use_droppy(int);
int main(int argc, char **) {
  int a = use_pod(argc);
  int b = use_droppy(argc);
  printf("sums %d %d\n", a, b);
  return 0;
}

// REJ-DF: pod.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of struct 'C' with a different shape in another translation unit
// REJ-PF: droppy.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of struct 'C' with a different shape in another translation unit

//--- keep.h
#pragma once
extern "C" int printf(const char *, ...);
struct Keeper {
  int v;
  ~Keeper() { printf("keeper out %d\n", v); }
  int get() const { return v; }
};

//--- keep-a.cpp
#include "keep.h"
int use_k1(int x) {
  Keeper k;
  k.v = x + 3;
  printf("k1 %d\n", k.get());
  return k.get();
}

//--- keep-b.cpp
#include "keep.h"
int use_k2(int x) {
  Keeper k;
  k.v = x + 8;
  printf("k2 %d\n", k.get());
  return k.get();
}

//--- keep-main.cpp
extern "C" int printf(const char *, ...);
int use_k1(int);
int use_k2(int);
int main(int argc, char **) {
  printf("ks %d %d\n", use_k1(argc), use_k2(argc));
  return 0;
}
