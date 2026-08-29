// FR-61f-c slice 1, DEFECT REGRESSION: the hoisted-cell map must be restored
// when a lifted `for` body FAILS mid-emit, or `--recover` hands the next
// function a dangling `mlir::Value`.
//
// `emitRangeFor` loads a body-invariant pointer's cells once in front of the
// `emitrust.for` and registers `cell -> loaded value` so that `loadPlace`
// returns the SSA value instead of emitting a `memref.load` inside the region.
// The entries are `Value`s into the function currently being emitted. A
// prototype that returned on `failed(bodyResult)` WITHOUT dropping them
// SEGFAULTED on `antirez/sds.c`: under `--recover` the rejected function's ops
// are ERASED, the map kept pointing into freed memory, and the very next
// function that read a pointer place got that freed value handed back. It
// compiled clean right up to the crash, so nothing but a crash test pins it.
//
// The reproducer needs three things in one translation unit, in order: a
// function whose lifted `for` body hoists a cursor and THEN fails (the
// `volatile` local is the rejection), recovery erasing that body, and a LATER
// function that reads a pointer place of its own. Both halves are checked --
// the first must become a stub, the second must still emit its own freshly
// loaded cursor and use it inside its own region.
//
// The fix is two-sided and both sides are load-bearing: `emitRangeFor` drops
// its entries on all three exits (success, body failure, staleness error), and
// `resetPerFunctionState` clears the whole map beside `placeBackedScalars` --
// under the comment that already warns of exactly this class of leak.
//
// RUN: emitrust-cc --recover --emit=import %s -o - 2>%t.err | FileCheck %s
// RUN: FileCheck %s --check-prefix=DIAG --input-file=%t.err

// The pointer reads come FIRST so EIGHT cursor cells are already hoisted and
// registered when the `volatile` local rejects the function. Eight, not one,
// on purpose: the crash needs the allocator to hand one of the freed cell
// `Operation`s straight back to a later function, and one stale entry is
// allocator luck (the one-cell form of this file did NOT crash the broken
// build; `antirez/sds.c` did, and eight distills it to thirteen lines).
int reject_after_hoist(const int *a, const int *b, const int *c, const int *d,
                       const int *e, const int *f, const int *g, const int *h,
                       int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s += a[i] + b[i] + c[i] + d[i] + e[i] + f[i] + g[i] + h[i];
    volatile int v = i;
    s += v;
  }
  return s;
}

// The next readers. Each cursor load must be its OWN, emitted into its own
// entry block; if a stale entry survived and its cell address was recycled,
// `loadPlace` would hand back the erased function's value and the next
// `Operation::create` faults on the freed operand.
int later_reader(const int *q, int n) {
  int t = 0;
  for (int i = 0; i < n; i++)
    t += q[i];
  return t;
}

int later_reader2(const int *q, const int *r, int n) {
  int t = 0;
  for (int i = 0; i < n; i++)
    t += q[i] * r[i];
  return t;
}

int later_reader3(const int *q, const int *r, const int *u, int n) {
  int t = 0;
  for (int i = 0; i < n; i++)
    t += q[i] + r[i] + u[i];
  return t;
}

// The rejected function is a stub carrying the verbatim rejection text; no
// part of the half-built body (its cells, its hoisted load, its region)
// survives.
// CHECK-LABEL: func.func @reject_after_hoist(
// CHECK-NEXT: emitrust.call_opaque "unimplemented!"()
// CHECK-SAME: unsupported: volatile-qualified type
// CHECK-NEXT: return
// CHECK-NEXT: }
// CHECK-NOT: memref.alloca

// The later function is untouched: its own cursor cell, its own load in front
// of its own `emitrust.for`, and that value used inside the region.
// CHECK-LABEL: func.func @later_reader
// CHECK:        %[[CUR:.*]] = memref.load {{.*}} : memref<i64>
// CHECK:        emitrust.for
// CHECK:          arith.addi %[[CUR]]
// CHECK-NOT:    memref.
// CHECK:        return

// CHECK-LABEL: func.func @later_reader2
// CHECK:        %[[CUR2:.*]] = memref.load {{.*}} : memref<i64>
// CHECK:        emitrust.for
// CHECK:        return

// CHECK-LABEL: func.func @later_reader3
// CHECK:        %[[CUR3:.*]] = memref.load {{.*}} : memref<i64>
// CHECK:        emitrust.for
// CHECK:        return

// DIAG: range-for-pointer-hoist-recover.c:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: volatile-qualified type (recovered: emitted an unimplemented!() stub with the mapped signature)
// DIAG-NOT: error:
// DIAG: recovered 1 rejected top-level item:
// DIAG: stubbed 'reject_after_hoist' [other] unsupported: volatile-qualified type
