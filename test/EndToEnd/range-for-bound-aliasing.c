// FR-61f-8, a DEFECT fix: clause 5's "HI is loop-invariant" proof was purely
// SYNTACTIC, so it could not see a write the body makes THROUGH A CALL.
//
// `emitrust.for` evaluates its bound ONCE, before the loop; C's `for` re-reads
// it every iteration. Clause 5 bridges that with `stmtWritesVar(body, var)` --
// a walk over the body's own text. A call in the body is opaque to that walk,
// so a callee that writes the bound was invisible and the loop lifted anyway.
//
// THIS WAS A MEASURED MISCOMPILE, not a theoretical one. `bound_written_by_
// callee` below lifted to `for i in 0..limit`, snapshotting the bound and
// running 5 times where C re-reads it and runs 3 -- clang printed 603, the
// emitted crate printed 1505. AND THE CRATE COMPILED CLEAN: no rustc
// diagnostic can see a dropped re-read, so only the byte-diff oracle caught
// it. That is why this file diffs against the native at three argument values
// rather than asserting on emitted text alone.
//
// The proof now admitted is the narrowest one that keeps the overwhelmingly
// common `for (i = 0; i < n; i++) { printf(..); }` shape: once the body can
// call anything, every variable the bound reads must be an automatic-storage,
// NON-ADDRESS-TAKEN INTEGER. Such a variable has no name and no address
// outside this frame, so no callee can reach it. Everything else -- a global,
// a `static`, an address-taken local, or any indirection at all (`*p`, `a[k]`,
// `s.n`, whose bases are pointer/array/record and so not integers) -- falls
// back to the `while` lowering, which re-reads the bound every iteration and
// is therefore correct for all of them.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/range_for_bound_aliasing > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/range_for_bound_aliasing a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/range_for_bound_aliasing a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

int printf(const char *, ...);

int limit = 5;
int calls = 0;

void shrink(void) {
  calls++;
  if (limit > 2)
    limit--;
}

// THE MISCOMPILE. The body never mentions `limit`; `shrink()` does. Snapshot
// the bound and the trip count is wrong.
// CHECK-LABEL: fn bound_written_by_callee
// CHECK-NOT:     for {{.*}} in
int bound_written_by_callee(int seed) {
  int s = 0;
  for (int i = 0; i < limit; i++) {
    s += i + seed;
    shrink();
  }
  return s * 100 + calls;
}

// THE SHAPE THAT MUST SURVIVE, and the reason the fix is narrow rather than
// "any call in the body refuses": `n` is an automatic-storage, non-address-
// taken integer, so no callee can reach it however many calls the body makes.
// CHECK-LABEL: fn plain_bound_with_call
// CHECK:         for {{i|_i}} in 0i32..n
int plain_bound_with_call(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    shrink();
    s += i;
  }
  return s;
}

// A CONSTANT bound is invariant no matter what the body calls: nothing to
// re-read. This one keeps lifting too.
// CHECK-LABEL: fn constant_bound_with_call
// CHECK:         for {{i|_i}} in 0i32..6i32
int constant_bound_with_call(int seed) {
  int s = 0;
  for (int i = 0; i < 6; i++) {
    shrink();
    s += i + seed;
  }
  return s;
}

// An ADDRESS-TAKEN local bound is the same hazard one step removed: the body
// hands `&cap` to a callee, which writes the bound through the pointer. The
// syntactic walk sees no assignment to `cap` anywhere in the body.
// CHECK-LABEL: fn bound_address_taken
// CHECK-NOT:     for {{.*}} in
void bump(int *p) {
  if (*p > 1)
    (*p)--;
}
int bound_address_taken(int seed) {
  int cap = 4;
  int s = 0;
  for (int i = 0; i < cap; i++) {
    s += i + seed;
    bump(&cap);
  }
  return s * 10 + cap;
}

int main(int argc, char **argv) {
  printf("%d\n", bound_written_by_callee(argc));
  printf("%d\n", bound_address_taken(argc));
  printf("%d\n", plain_bound_with_call(argc + 4));
  printf("%d\n", constant_bound_with_call(argc));
  printf("limit=%d calls=%d\n", limit, calls);
  return 0;
}
