// FR-116 DEFECT regression, manifestation 2: a global bumped by a FREE
// FUNCTION that is CALLED FROM A C++ METHOD BODY. Here the plan is right
// about the global -- `bump` really is the sole owner, so `GActor` is a
// legitimate actor -- but renaming `bump` into `GActor::bump` leaves the
// method body calling a free function that no longer exists, and the crate
// died at rustc with `error[E0425]: cannot find function 'bump' in this
// scope`. Method call sites are invisible to the plan (methods are not
// item-graph nodes) AND to the pass's SymbolTable use queries (an
// `emitrust.impl` is a nested symbol table), so nothing caught it.
//
// Pins: the demotion warning is LOCATED ON THE METHOD-BODY CALL SITE, not
// on the arm's definition; the emitted Rust keeps `bump` a free function
// next to a thread-local global with no GActor anywhere; and -- the
// CONTAINMENT pin -- `solo`, whose owner is never named from a method,
// still lifts into a real `SoloActor`. The conservative shape's cost is
// per-cluster, not program-wide.
// RUN: emitrust-cc --emit=rust %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --check-prefix=WARN < %t.err
// RUN: FileCheck %s --check-prefix=RUST --implicit-check-not=GActor < %t.rs
//
// WARN: actor-lift-cpp-method-arm.cpp:[[#@LINE+25]]:{{[0-9]+}}: warning: actor lift: demoted GActor: arm 'bump' is referenced from a method body
//
// RUST: thread_local!
// RUST: static G: std::cell::Cell<i32>
// RUST: fn bump(d: i32) -> i32
// RUST: struct SoloActor
// RUST: impl SoloActor
// RUST: fn solo_bump(&mut self, d: i32) -> i32
// RUST: fn s_step(&mut self) -> i32
// RUST: bump(self.v)

extern "C" int printf(const char *, ...);

int g = 7;

int bump(int d) { g = g + d; return g; }

// Never named from a method body: this cluster must keep lifting.
int solo = 100;

int solo_bump(int d) { solo = solo + d; return solo; }

struct S {
  int v;
  S(int x) : v(x) {}
  int step() { return bump(v); }
};

int main(int argc, char **) {
  S s(argc);
  printf("%d %d\n", s.step(), solo_bump(argc));
  return 0;
}
