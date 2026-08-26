// FR-61f incr. 1: a range-eligible `for` body may touch an AGGREGATE -- a
// by-value struct or union, an array of struct, a multi-dimensional array, an
// array of non-integer element -- or a function pointer.
//
// The old whitelist rejected all of these, and its stated reason was that such
// a variable would be backed by a `memref.alloca` cell inside the single-block
// region (the real 61f-0 blocker). That reason simply does not hold for these
// types: `emitLocalVar` computes `isAggregate` from
// `isa<StructType, ArrayType>`, and `mapType` sends EVERY complete record
// (union included -- it imports as a one-field struct) and EVERY
// `ConstantArrayType` regardless of element type to exactly those two, so all
// of them already took the `createVariablePlace` branch and never produced an
// alloca. Function pointers likewise, via `isPlaceOnly`. So this is a gate
// relaxation with no new place-emission machinery -- the whitelist was simply
// STALE. The array clause was also needlessly narrow: it demanded an INTEGER
// element, which rejected `int[3][4]` (whose element `int[4]` is not an
// integer) and every array of struct.
//
// The NESTED-LOOP relaxation ships with it and must, because measurement said
// so: the aggregate widening ALONE lifted 39 corpus loops but degraded 12
// ENCLOSING loops from a clean `while` to `loop { let c = ..; if c { .. } if
// !c { break; } }`. Lifting an inner loop drags the outer induction into
// `placeBackedScalars`, and a place gives `lift-cf-to-scf` no SSA
// loop-carried value to build an `scf.while` from. The fix is to let the
// OUTER loop lift too: a nested `for` that itself matches emits a nested
// REGION op, not cf blocks, so it never breaks the single-block invariant.
// Corpus effect of the pair: `for .. in` 76 -> 125, `while` 196 -> 141,
// `loop {` 177 -> 183, lines 22342 -> 22319.
//
// A float SCALAR is genuinely cell-backed and still falls back, and so does a
// nested loop that does NOT itself lift: those are the frontiers this test
// pins from the other side.
//
// Every case is byte-diffed against the clang-built native at several argument
// values, so nothing constant-folds and a lift that dropped or reordered a
// value shows up as a wrong number rather than a compile error. `cargo build`
// success cannot see a miscompile.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/range_for_aggregates > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/range_for_aggregates a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/range_for_aggregates a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

int printf(const char *, ...);

struct P { int x, y; };
union U { int i; unsigned u; };

// A by-value struct READ and WRITTEN in the body: the place lives in the
// enclosing block, so the write is observable after the loop.
// CHECK-LABEL: fn sum_struct
// CHECK:         for {{i|_i}} in 0i32..
// CHECK-NOT:     while
int sum_struct(int n) {
  struct P p;
  int s = 0;
  p.x = 3;
  p.y = 4;
  for (int i = 0; i < n; i++) {
    s += p.x * i + p.y;
    p.x = p.x + 1;
  }
  return s + p.x;
}

// An array OF STRUCT: `struct P[4]` is an `!emitrust.array` place, but the old
// integer-element clause rejected it.
// CHECK-LABEL: fn arr_struct
// CHECK:         for {{i|_i}} in 0i32..
int arr_struct(int n) {
  struct P a[4];
  int s = 0;
  for (int i = 0; i < 4; i++) {
    a[i].x = i;
    a[i].y = i * 2;
  }
  for (int i = 0; i < n; i++)
    s += a[i & 3].x + a[i & 3].y;
  return s;
}

// A MULTI-DIMENSIONAL array: the element of `int[3][4]` is `int[4]`, which is
// not an integer, so the old clause rejected it even though the mapped type is
// an `!emitrust.array` of `!emitrust.array` and hence a place.
// CHECK-LABEL: fn multidim
// CHECK:         for {{i|_i}} in 0i32..
int multidim(int n) {
  int m[3][4];
  int s = 0;
  for (int i = 0; i < 12; i++)
    m[i / 4][i % 4] = i * 10;
  for (int i = 0; i < n; i++)
    s += m[i % 3][i % 4];
  return s;
}

// An array of DOUBLE. The array itself is a place; only a float SCALAR is
// cell-backed (see `float_scalar_falls_back` below).
// CHECK-LABEL: fn arr_double
// CHECK:         for {{i|_i}} in 0i32..
double arr_double(int n) {
  double d[5];
  double s = 0.0;
  for (int i = 0; i < 5; i++)
    d[i] = (double)i * 1.5;
  for (int i = 0; i < n; i++)
    s += d[i % 5];
  return s;
}

// A by-value UNION: it imports as a one-field struct, so it maps to the same
// `!emitrust.struct` any record does, and is a place for the same reason.
// CHECK-LABEL: fn use_union
// CHECK:         for {{i|_i}} in 0i32..
int use_union(int n) {
  union U u;
  int s = 0;
  u.i = 0;
  for (int i = 0; i < n; i++) {
    u.i = i;
    s += (int)(u.u & 7);
  }
  return s;
}

int twice(int v) { return v * 2; }
int thrice(int v) { return v * 3; }

// FUNCTION POINTERS read, called, and reassigned in the body: `!emitrust.fn_ptr`
// is `isPlaceOnly`, never a cell (a memref of a dialect type is illegal).
// CHECK-LABEL: fn fnptr_swap
// CHECK:         for {{i|_i}} in 0i32..
int fnptr_swap(int n) {
  int (*f)(int) = twice;
  int (*g)(int) = thrice;
  int s = 0;
  for (int i = 0; i < n; i++) {
    int (*t)(int) = f;
    s += f(i);
    f = g;
    g = t;
  }
  return s;
}

// THE FRONTIER FROM THE OTHER SIDE: a float SCALAR really is backed by a
// `memref.alloca` cell, which mem2reg cannot promote across the region op, so
// this must keep falling back to the CFG `while`. No corpus loop is blocked by
// one, so relaxing it is not worth its own increment.
// CHECK-LABEL: fn float_scalar_falls_back
// CHECK-NOT:     for {{.*}} in
// CHECK:         while
double float_scalar_falls_back(int n) {
  double acc = 0.0;
  for (int i = 0; i < n; i++)
    acc = acc + (double)i * 0.5;
  return acc;
}

// NESTED LOOPS, both lifting: the inner `emitrust.for` is a region op inside
// the outer one's single-block region, and the inner body reads the OUTER
// induction (which is what `emitRangeFor`'s save/restore of `inductionValues`
// exists for).
// CHECK-LABEL: fn nested_pair
// CHECK:         for {{i|_i}} in 0i32..
// CHECK:         for {{j|_j}} in 0i32..
// CHECK-NOT:     while
int nested_pair(int n) {
  int g[4][6];
  int s = 0;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 6; j++)
      g[i][j] = i * 6 + j;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 6; j++)
      s += g[i][j] * ((i + j + n) & 1);
  return s;
}

// THE OTHER FRONTIER: the inner loop has an `if` in its body, so the inner
// loop does not lift -- and therefore the OUTER loop must not lift either,
// because the inner one would emit the cf `while` lowering inside the outer
// single-block region. Both fall back.
// CHECK-LABEL: fn nested_inner_blocked
// CHECK-NOT:     for {{.*}} in
// CHECK:         while
int nested_inner_blocked(int n) {
  int g[4][6];
  int s = 0;
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 6; j++) {
      if (((i + j + n) & 1) != 0)
        s += i * 6 + j;
    }
  }
  (void)g;
  return s;
}

int main(int argc, char **argv) {
  int n = argc + 6;
  printf("%d\n", sum_struct(n));
  printf("%d\n", arr_struct(n));
  printf("%d\n", multidim(n));
  printf("%.3f\n", arr_double(n));
  printf("%d\n", use_union(n));
  printf("%d\n", fnptr_swap(n));
  printf("%.3f\n", float_scalar_falls_back(n));
  printf("%d\n", nested_pair(n));
  printf("%d\n", nested_inner_blocked(n));
  return 0;
}
