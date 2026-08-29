// RUN: emitrust-import-c %s | FileCheck %s

// FR-149 REGRESSION GUARD. An ANONYMOUS enum (`typedef enum { ... } T;`,
// no tag) has no Rust counterpart: `mapType` gives it a plain `i32` and
// its enumerators are plain `i32` constants. That path ALREADY worked --
// it is precisely why four synthetic reductions of the systemd defect
// failed to reproduce it -- so the FR-149 enum-index normalization must
// leave it byte-for-byte alone: the index reaches `emitrust.subscript`
// as a bare `i32` with NO `emitrust.cast` interposed. A cast appearing
// here would mean the seam started converting values that were never
// enum-typed in the IR.

typedef enum { A = 0, B = 1, C = 2 } Anon;

static const int tbl[3] = {10, 20, 30};

int anon_index(Anon t) { return tbl[t]; }

// The parameter itself is a plain i32, so nothing enum-shaped exists.
// CHECK-LABEL: func.func @anon_index(%arg0: i32) -> i32
// CHECK: %[[I:.*]] = memref.load
// CHECK-NEXT: emitrust.subscript %{{[0-9]+}}[%[[I]]] : (!emitrust.lvalue<!emitrust.array<3xi32>>, i32) -> !emitrust.lvalue<i32>

// A plain integer index is likewise untouched: no cast, straight into the
// subscript.
int int_index(int i) { return tbl[i]; }

// CHECK-LABEL: func.func @int_index
// CHECK: %[[J:.*]] = memref.load
// CHECK-NEXT: emitrust.subscript %{{[0-9]+}}[%[[J]]] : (!emitrust.lvalue<!emitrust.array<3xi32>>, i32) -> !emitrust.lvalue<i32>

// CHECK-NOT: emitrust.cast

int main(void) { return anon_index(B) + int_index(2); }
