// REQUIRES: cargo
// Intent: pin the CAPABILITY BOUNDARY of the negative-displacement fence on
// the multi-object (Phase-1b slice) pointer lowering — everything that is
// NOT a negative index below a slice parameter's origin must keep importing
// and must keep byte-diffing against the native.
//
// Two local arrays put every pointer class on the slice lowering (owner
// promotion needs exactly ONE storage base), so each pointer parameter here
// is a `&mut [T]` re-based at the pointee. What the fence must NOT touch:
//   - a non-negative constant or runtime index through a slice parameter,
//   - a cursor walked FORWARD then backward inside the callee while staying
//     at or above the parameter's origin,
//   - a negative index through a pointer LOCAL, whose base place is the
//     whole array rather than a re-based slice, so element -1 of the cursor
//     is a real element of the object.
// The refused shape itself lives in
// test/Import/C/pointer-param-negative-index-invalid.c.
//
// Seeds come from argc so constant folding cannot pre-compute the answers
// and hide a miscompile; stdout is byte-diffed against the clang native.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name pointer_param_negative_index_slice --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/pointer_param_negative_index_slice > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

/* Non-negative index through a slice parameter. */
static int at(const int *p, int k) { return p[k]; }

/* Constant non-negative displacements, both spellings. */
static int head(const int *p) { return p[0] + p[1]; }
static int head_deref(const int *p) { return *(p + 1); }

/* A cursor walked forward and then back down to the parameter's own
   origin — never below it. */
static int fold_back(const int *p, int n) {
  const int *q = p + n;
  int s = 0;
  while (q > p) {
    q--;
    s = s * 10 + *q;
  }
  return s;
}

/* A write through a non-negative index on a mutable slice parameter. */
static void add_at(int *p, int k, int by) { p[k] = p[k] + by; }

int main(int argc, char **argv) {
  int a[3];
  int b[3];
  int i;
  const int *pa;
  const int *pb;
  for (i = 0; i < 3; i++) {
    a[i] = (i + 1) * 10 + argc;
    b[i] = (i + 1) + argc;
  }
  printf("%d %d\n", at(a, argc), at(b, argc + 1));
  printf("%d %d\n", head(a), head(b));
  printf("%d %d\n", head_deref(a), head_deref(b));
  printf("%d %d\n", fold_back(a, 3), fold_back(b, 3));
  add_at(a, argc, argc * 5);
  add_at(b, argc - 1, argc * 5);
  printf("%d %d %d %d %d %d\n", a[0], a[1], a[2], b[0], b[1], b[2]);
  /* Negative index through a pointer LOCAL: the base place is the whole
     array, so the displacement resolves to a real element. */
  pa = &a[2];
  pb = &b[argc];
  printf("%d %d\n", pa[-2], pb[-1]);
  return 0;
}
