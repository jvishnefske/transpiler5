// FR-61f-c slice 1: a counted `for` whose body READS a pointer lifts to
// `emitrust.for`, and the pointer's decomposition cells are LOADED ONCE in
// front of the region.
//
// This pins the mechanism, which is the whole point of the slice. A pointer
// local/parameter is not place-backed: it decomposes into entry-block
// `memref` CELLS (element cursor, nullable discriminant, enum-of-bases
// discriminant). MLIR's mem2reg refuses to promote a slot whose uses live in a
// nested region unless the parent op implements `PromotableRegionOpInterface`,
// and `emitrust.for` implements none -- so a cursor read INSIDE the region
// leaves a `memref.alloca` that `convert-to-emitrust` rejects outright. Before
// this slice, `rangeForBodyVarIsPlaceBacked` therefore refused every pointer
// body variable and the loop degraded to the cf `while` lowering.
//
// design.md's prescription was to convert those cells into
// `emitrust.variable` places; that was measured WORSE (a place is opaque to
// `canonicalize`, so it renders a dead `let` plus unfolded cursor arithmetic).
// The mechanism taken instead is HOISTING: `emitRangeFor` emits the
// `memref.load` before the `emitrust.for` and `loadPlace` hands back that SSA
// value inside. So the load-bearing assertion in every admit leg below is
// `%CUR` defined OUTSIDE the region and used INSIDE it, with NO memref op left
// in the region at all.
//
// Hoisting is sound only while the pointer's state cannot change across
// iterations, so admission is exactly `!stmtWritesVar(body, var) &&
// !addressTaken.contains(var)` -- plus two shapes whose state simply is not
// reachable through `pointerLocals`. Every refusal leg is pinned from the
// other side: no `emitrust.for`, back to the cf `while`. Widening any of them
// must come with the `blocksRangeForLift` pointer leak fixed first (admitting
// a MUTATED pointer segfaults `verifyNSuccessors` on three corpus files).
//
// RUN: emitrust-import-c %s | FileCheck %s

// ADMIT: the dominant corpus shape -- a read-only cursor PARAMETER walked by
// index. It has no `PointerRegion` at all (it is never *bound*), which is why
// the region model could never have unlocked it and the admission clause is
// stated on the clang AST alone.
// CHECK-LABEL: func.func @sum_param
// CHECK:         %[[CUR:.*]] = memref.load {{.*}} : memref<i64>
// CHECK:         emitrust.for
// CHECK:           arith.addi %[[CUR]]
// CHECK-NOT:     memref.
// CHECK:         return
int sum_param(const int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}

// ADMIT: a NULLABLE region (CTS-P8). BOTH cells hoist -- the i64 cursor and
// the i1 Option discriminant -- so the null check renders as an `assert!` on a
// value computed outside the loop and the region still holds no cf edge.
// CHECK-LABEL: func.func @sum_nullable
// CHECK:         %[[NCUR:.*]] = memref.load {{.*}} : memref<i64>
// CHECK:         %[[NN:.*]] = memref.load {{.*}} : memref<i1>
// CHECK:         emitrust.for
// CHECK:           arith.addi %[[NCUR]]
// CHECK:           emitrust.call_opaque "assert!"(%[[NN]])
// CHECK-NOT:     memref.
// CHECK-NOT:     cf.br
// CHECK:         return
int sum_nullable(int n, int pick) {
  static int arr[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  int *p = 0;
  if (pick)
    p = arr;
  int s = 0;
  for (int i = 0; i < n; i++)
    s += p[i];
  return s;
}

// ADMIT: a WRITE THROUGH the pointer. The pointer itself is invariant -- only
// the region it designates is mutated -- so the hoisted cursor stays correct.
// CHECK-LABEL: func.func @fill_through
// CHECK:         %[[FCUR:.*]] = memref.load {{.*}} : memref<i64>
// CHECK:         emitrust.for
// CHECK:           arith.addi %[[FCUR]]
// CHECK:           emitrust.assign
// CHECK-NOT:     memref.
// CHECK:         return
void fill_through(int *out, int n) {
  for (int i = 0; i < n; i++)
    out[i] = i * i;
}

// ADMIT: a string-literal region. The backing is a `const`-marked
// `emitrust.variable` place already; only the cursor cell needed hoisting.
// CHECK-LABEL: func.func @sum_literal
// CHECK:         %[[LCUR:.*]] = memref.load {{.*}} : memref<i64>
// CHECK:         emitrust.for
// CHECK:           arith.addi %[[LCUR]]
// CHECK-NOT:     memref.
// CHECK:         return
int sum_literal(void) {
  const char *t = "hello";
  int s = 0;
  for (int i = 0; i < 5; i++)
    s += t[i];
  return s;
}

// REFUSE: the pointer is WALKED in the body (`p++`). Its cursor changes every
// iteration, so one hoisted load would be stale from the second iteration on --
// a silent miscompile, which is why `stmtWritesVar` gates admission and why
// `emitRangeFor` additionally walks the finished region for a store to any
// hoisted cell. Falls back to the cf `while` lowering, unchanged.
// CHECK-LABEL: func.func @walk_ptr
// CHECK-NOT:     emitrust.for
// CHECK:         cf.br
int walk_ptr(const int *a, int n) {
  const int *p = a;
  int s = 0;
  for (int i = 0; i < n; i++) {
    s += *p;
    p++;
  }
  return s;
}

// REFUSE: the mutation is in the INNER loop of a nested pair. `stmtWritesVar`
// RECURSES, so the outer loop sees it too and neither loop lifts. A syntactic
// walk that stopped at the inner `for` would have admitted the outer one and
// hoisted a cursor the inner loop then advances.
// CHECK-LABEL: func.func @nested_inner_write
// CHECK-NOT:     emitrust.for
// CHECK:         cf.br
int nested_inner_write(const int *a, int n, int m) {
  const int *r = a;
  int s = 0;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < m; j++) {
      s += *r;
      r++;
    }
  }
  return s;
}

// REFUSE: `&p` is taken, so the body writes `p` through `*pp` without any
// syntactic assignment to `p`. `stmtWritesVar` alone cannot see that; the
// function-wide `addressTaken` set is the second half of the clause and fences
// it. This is the `pointers-ptr-to-ptr.c` second-order shape.
// CHECK-LABEL: func.func @addr_taken
// CHECK-NOT:     emitrust.for
// CHECK:         cf.br
int addr_taken(int *a, int n) {
  int *p = a;
  int **pp = &p;
  int s = 0;
  for (int i = 0; i < n; i++) {
    *pp = a + i;
    s += **pp;
  }
  return s;
}

// FRONTIER: a pointer DECLARED INSIDE the body. Its initializing store is
// inside the region, so there is no value to load in front of it -- hoisting
// is not merely unhelpful here, it has nothing to hoist. This is the FR-61f-c
// slice-3 residue (a ~19-site ceiling across four corpora, and the only shape
// that genuinely needs the place conversion); it keeps the `while` lowering it
// works under today rather than lifting into the located `memref.alloca`
// legalization error.
// CHECK-LABEL: func.func @decl_in_body
// CHECK-NOT:     emitrust.for
// CHECK:         cf.br
int decl_in_body(const int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    const int *p = a + i;
    s += *p;
  }
  return s;
}
