// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/tu-a.cpp %t/tu-b.cpp 2>&1 | FileCheck %s

// FR-114: the cross-TU ASYMMETRIC overload declaration case must stay a
// LOUD failure. The free-function overload suffix is a pure function of
// ONE translation unit's AST (DeclContext::lookup sees only the current
// TU's declarations), so a TU that declares only PART of an overload set
// computes a DIFFERENT symbol than the TU that defines all of it:
// tu-a defines g(int)/g(double) and emits `g_i32`/`g_d`; tu-b sees a
// lone `int g(int);` prototype — an overload "set" of size 1 — and
// resolves its call against the BARE symbol `g`, which no TU defines.
//
// The safe failure direction is the existing whole-program backstop: an
// external prototype that survives to finalization without a definition
// is a located, loud rejection ("referenced but not defined in any
// translation unit"), never a silent link of tu-b's call onto one of
// tu-a's overloads by luck of the suffix. This file pins that the
// backstop actually catches the asymmetric shape — the same backstop the
// FR-114 spike recorded as the acceptance for the prototype-only silent
// merge hazard (uncalled colliding prototypes vanish; a REFERENCED one
// must land here).
//
// CHECK: tu-b.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function 'g' is referenced but not defined in any translation unit

//--- tu-a.cpp
int g(int x) { return x * 2; }
double g(double x) { return x * 2.0; }

//--- tu-b.cpp
int g(int x);
int use_g(int v) { return g(v); }
