// FR-152, the out-of-scope half: a BOUNDED loop whose body-declared place
// escapes through an inner `return` stays a located rejection, and this file
// pins that it is NOT quietly admitted by the FR-152 hoist.
//
// The two shapes look identical in C but not after `lift-cf-to-scf`. For an
// unbounded loop the `emitrust.variable` sits directly in the `scf.while`
// before-region, so moving it in front of the loop makes the carried value
// loop-INVARIANT and the canonicalizer removes it. For a bounded loop the lift
// nests the variable one region deeper -- inside an `scf.if` in the
// before-region -- and seeds the carried value with
// `ub.poison : !emitrust.lvalue<T>`. That value is not loop-invariant, so no
// amount of hoisting lets canonicalize remove it; a generalized hoist was
// spiked and measurably does not fix it.
//
// So this must keep failing, loudly and with a location. The FR-152 pass is
// deliberately narrow (`scf.while` region only), and if a future widening
// makes this compile, that compile has to be justified by a byte-diff test --
// not by this test going green on its own.
// RUN: not emitrust-cc --emit=rust %s -o - 2>&1 | FileCheck %s

int f(int k, int n) {
  // The `ub.poison` seeding the carried lvalue is materialized at the top of
  // the function, so the location is `f`'s own line -- located, which is the
  // property being pinned, even though it is coarser than the FR-152 fence's.
  // CHECK: loop-escaping-place-bounded-reject.c:[[#@LINE-4]]:{{[0-9]+}}: error: failed to legalize operation 'ub.poison' that was explicitly marked illegal
  for (int i = 0; i < k; i++) {
    unsigned char c = (unsigned char)(n + i);
    if (i == 2)
      return (int)c;
  }
  return -1;
}

int main(void) { return f(5, 10); }
