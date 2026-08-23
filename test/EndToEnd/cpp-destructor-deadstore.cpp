// REQUIRES: cargo
// W2.17 REGRESSION PIN for the has_drop LIVENESS GUARD -- the single
// highest-value test of the wave, because the shape it pins was measured
// SILENTLY MISCOMPILING and compiling clean.
//
// `R a; a.id = argc + 4;` with no subsequent read of `a` is, to the
// emitter's dead-store / deferred-initialization analysis, a binding whose
// synthesized default AND whose only store are both dead: it used to emit
// `let _a: R;` and drop the store entirely. An UNINITIALIZED Rust binding
// is NEVER dropped, so with a `Drop` impl in play that elision deletes the
// destructor's side effects outright (measured: clang++ printed `dtor 5`,
// the hand-driven Rust printed nothing).
//
// With `emitrust.has_drop` on the struct, `drop()` is a read of every
// field on every path, so no store to such an object is ever dead and the
// initializer can never be deferred. This test is the byte-diff that says
// so; `cargo build` alone cannot see this failure at all.
//
// Both the function-scope object and the branch-body object are covered,
// and every value derives from argc.
//
// Since FR-111 both sites render as the base-free all-fields fuse
// (`let a: R = R { id: .. };`) -- the store is folded into the literal, not
// dropped, and this byte-diff is the oracle that the drop still fires.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_destructor_deadstore > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct R {
  int id;
  ~R() { printf("dtor %d\n", id); }
};

int main(int argc, char **argv) {
  R a;
  a.id = argc + 4; // never read again: a dead store to everything but drop()
  if (argc > 0) {
    R b;
    b.id = argc + 40; // ditto, inside a branch body
  }
  printf("hi %d\n", argc);
  return 0;
}
