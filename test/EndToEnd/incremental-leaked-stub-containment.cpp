// REQUIRES: cargo
// FR-128 end-to-end claim: containing a leaked pass-1 ctor stub at its
// REFERENCING items (the rollback no longer re-materializes stubs born
// inside a failed template item) must not perturb one byte of the healthy
// code the program actually executes. Before the fix this input produced
// NO crate at all under --incremental: the `__int128` sibling spec killed
// the BasicFp template item, the restored u64 ctor stub let `op_mul` and
// `contained_user` import against a body-less callee, and finalize died
// with the whole-crate `referenced but not defined` error. Now the
// contained items are unimplemented!() stubs that main never calls, and
// the crate's stdout is byte-diffed against the clang++ native -- the only
// oracle that can see a miscompile in `healthy`, since a compile-clean
// cargo build proves nothing about values.
//
// Every printed value derives from argc, so constant folding cannot
// pre-compute the answers and hide a miscompile behind a clean build.
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate --build \
// RUN:   2>%t.err
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/incremental_leaked_stub_containment > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

template <typename F> struct BasicFp {
  F f;
  int e;
  constexpr BasicFp() : f(0), e(0) {}
  constexpr BasicFp(unsigned long long f_val, int e_val) : f(f_val), e(e_val) {}
};
using Fp = BasicFp<unsigned long long>;

// --- Contained, never executed: the W2.25 operator constructing the
// --- leaked-stub type, a direct toucher of the failed sibling spec, and
// --- the plain caller that used to be the crate-killer.
inline auto operator*(Fp x, Fp y) -> Fp { return {x.f * y.f, x.e + y.e + 64}; }
inline int touch_other() {
  BasicFp<unsigned __int128> g;
  return g.e;
}
int contained_user(int k) {
  Fp a{2ULL, 1};
  Fp b{3ULL, k};
  Fp c = a * b;
  return c.e + touch_other();
}

// --- Healthy and executed: must survive the containment untouched.
int healthy(int a, int b) { return a * b + 7; }

int main(int argc, char **) {
  int s = argc; // 1 on a bare run, but the compiler cannot know that.
  printf("healthy=%d\n", healthy(s + 5, s + 6));
  printf("sum=%d\n", healthy(s, s + 1) + healthy(s + 2, s + 3));
  printf("mix=%d\n", healthy(s * 3, 9 - s) - healthy(s, s));
  return 0;
}
