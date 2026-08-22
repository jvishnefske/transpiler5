// REQUIRES: cargo
// FR-116, manifestations 2/3/4, end to end: the actor SURFACE reached from
// a C++ method body. Here the plan's ownership analysis is correct -- the
// globals really are owned by free functions -- but the lift RENAMES those
// functions into actor methods, and the method-body call sites that still
// name them are invisible to both the plan and the pass's SymbolTable
// queries. Each manifestation reached rustc as broken Rust rather than a
// diagnostic:
//   * an ARM called from a method     -> error[E0425]: cannot find function
//   * a CROSS CLIENT called from a method -> error[E0061]: this function
//     takes 3 arguments but 1 argument was supplied (the lift prepends one
//     `&mut` per actor; the method's call site passes none)
//   * a FUNCTION POINTER to an arm taken in a method -> error[E0425]
// The fix demotes those actors. `solo_bump` is the CONTAINMENT control: it
// is never named from a method, so its cluster is free to keep lifting --
// the cost of the conservative shape is per-cluster, not program-wide.
//
// A crate that builds proves nothing here (all three manifestations changed
// which symbols exist), so the oracle is the byte diff of stdout against a
// `clang++ -std=c++17` build of the identical source. Every value derives
// from argc, so no constant folding can pre-compute the answers.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_method_global_arm > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

// Manifestation 2: a real arm, called from a method body.
int armg = 1;
int bump(int d) {
  armg = armg + d;
  return armg;
}

// Manifestation 3: a genuine cross client (empty direct footprint, reads
// two actors through its callees), called from a method body.
int left_g = 0;
int right_g = 0;
int bump_left(void) { left_g += 1; return left_g; }
int bump_right(void) { right_g += 3; return right_g; }
int get_left(void) { return left_g; }
int get_right(void) { return right_g; }
int observe(int k) { return get_left() + get_right() + k; }

// Manifestation 4: an arm referenced through a function POINTER taken
// inside a method body -- an opaque text reference with no symbol to query.
int fpg = 2;
int fbump(int d) {
  fpg = fpg + d;
  return fpg;
}

// The containment control: never named from a method body.
int solo = 100;
int solo_bump(int d) {
  solo = solo + d;
  return solo;
}

struct S {
  int v;
  S(int x) : v(x) {}
  int step() { return bump(v); }
  int go() { return observe(v); }
  int viafp() {
    int (*f)(int) = fbump;
    return f(v);
  }
};

int main(int argc, char **) {
  S s(argc);
  int a = s.step();
  int a2 = s.step();
  bump_left();
  bump_right();
  int b = s.go();
  int c = s.viafp();
  int d = solo_bump(argc);
  int e = solo_bump(argc + 2);
  printf("a=%d a2=%d b=%d c=%d d=%d e=%d armg=%d fpg=%d solo=%d l=%d r=%d\n", a,
         a2, b, c, d, e, armg, fpg, solo, left_g, right_g);
  return 0;
}
