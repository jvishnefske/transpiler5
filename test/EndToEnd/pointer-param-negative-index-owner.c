// REQUIRES: cargo
// Intent: pin the OWNER (single-storage-base) pointer lowering's handling of
// a NEGATIVE displacement through a pointer parameter — `p[-1]`, `p[-2]` and
// a runtime-negative `p[-k]` — against the clang-built native binary.
//
// This is one half of a two-model invariant. A pointer parameter whose class
// unifies to exactly ONE local storage base is promoted to an owner method
// whose parameter is an i64 element CURSOR into the receiver's whole data
// array (`self.data[(p + -1i64) as usize]`), so a displacement is added to
// the cursor in i64 BEFORE the `as usize` cast and a negative one resolves to
// a real element. The other half — the multi-object slice lowering, where the
// parameter is a `&mut [T]` already re-based at the pointee and the earlier
// elements are simply not in the slice — is refused at import time and pinned
// in test/Import/C/pointer-param-negative-index-invalid.c.
//
// Every access below is IN BOUNDS: `prev(&a[1])` reads a[0], `prev2(&a[3])`
// reads a[1], `back(&a[2], 1)` reads a[1]. This is well-defined C, so a panic
// is a wrong ANSWER, not a safe failure — and `cargo build` cannot see it.
// The seeds are derived from argc so constant folding cannot pre-compute the
// answer and hide a miscompile; stdout is byte-diffed against the native.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name pointer_param_negative_index_owner --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointer_param_negative_index_owner > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

/* Constant negative displacements through a pointer parameter. */
static int prev(const int *p) { return p[-1]; }
static int prev2(const int *p) { return p[-2]; }

/* The pointer-arithmetic spelling of the same access. */
static int prev_deref(const int *p) { return *(p - 1); }

/* A RUNTIME-negative displacement: the cursor arithmetic, not a folded
   constant, is what has to carry the sign. */
static int back(const int *p, int k) { return p[-k]; }

/* A negative displacement written into, then read back through the same
   parameter: the place must be an lvalue on the owner's array. */
static void bump_prev(int *p, int by) { p[-1] = p[-1] + by; }

int main(int argc, char **argv) {
  int a[4];
  int i;
  for (i = 0; i < 4; i++)
    a[i] = (i + 1) * 10 + argc;
  printf("%d %d\n", prev(&a[1]), prev2(&a[3]));
  printf("%d %d\n", prev_deref(&a[2]), back(&a[2], argc));
  bump_prev(&a[3], argc * 7);
  printf("%d %d %d %d\n", a[0], a[1], a[2], a[3]);
  return 0;
}
