// RUN: emitrust-import-c %s | FileCheck %s

// CTS-P6: a local pointer bound into a global aggregate. The pointer's
// cursor stays a local i64 cell (exactly the Phase-1a decomposition), so
// no borrow of the global is ever stored; every element access through
// the pointer stages the global's whole value in a local copy exactly
// like a direct global element access, and write contexts store the
// modified copy back (load-modify-store, exact for the single-threaded C
// subset). A write through the pointer is therefore visible to the next
// direct read of the global, and vice versa.

int gx;
int garr[4];

// Decay bind (`p = garr`) starts the cursor at 0; ++ and += walk it; a
// read stages the array and subscripts the staged copy at the cursor.
// CHECK-LABEL: func.func @walk_read
// CHECK: %[[CELL:.*]] = memref.alloca() : memref<i64>
// CHECK: memref.store %{{.*}}, %[[CELL]][] : memref<i64>
// CHECK: arith.addi
// CHECK: memref.store %{{.*}}, %[[CELL]][] : memref<i64>
// CHECK: %[[CUR:.*]] = memref.load %[[CELL]][] : memref<i64>
// CHECK: %[[COPY:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: %[[VAL:.*]] = emitrust.global_load @garr : !emitrust.array<4xi32>
// CHECK: emitrust.assign %[[COPY]] = %[[VAL]]
// CHECK: %[[ELEM:.*]] = emitrust.subscript %[[COPY]][%[[CUR]]]
// CHECK: emitrust.load %[[ELEM]]
// CHECK-NOT: emitrust.addr_of
int walk_read(void) {
  int *p = garr;
  p++;
  return *p;
}

// `q = &garr[i]` stores the runtime index into the cursor cell; a write
// through the pointer stages the array, assigns the element, and stores
// the whole staged copy back into the global (the writeback), sequenced
// before the next statement's direct global read, which stages afresh
// and so observes the written value.
// CHECK-LABEL: func.func @write_then_direct_read
// CHECK: memref.store %{{.*}}, %{{.*}}[] : memref<i64>
// CHECK: emitrust.global_load @garr : !emitrust.array<4xi32>
// CHECK: emitrust.subscript
// CHECK: emitrust.assign
// CHECK: emitrust.global_store %{{.*}}, @garr : !emitrust.array<4xi32>
// CHECK: emitrust.global_load @garr : !emitrust.array<4xi32>
// CHECK: emitrust.subscript
int write_then_direct_read(int i, int v) {
  int *q = &garr[i];
  *q = v;
  return garr[2];
}

// A compound write through the pointer (`p[i] += v`) is a staged
// load-modify-store with the same writeback.
// CHECK-LABEL: func.func @compound_write
// CHECK: emitrust.global_load @garr : !emitrust.array<4xi32>
// CHECK: emitrust.subscript
// CHECK: arith.addi
// CHECK: emitrust.global_store %{{.*}}, @garr : !emitrust.array<4xi32>
void compound_write(int i, int v) {
  int *p = garr;
  p[i] += v;
}

// The degenerate form: `s = &gx` needs no cursor cell at all, and `*s`
// stages the scalar (writes store it back).
// CHECK-LABEL: func.func @scalar_write
// CHECK-NOT: memref.alloca() : memref<i64>
// CHECK: emitrust.global_load @gx : i32
// CHECK: emitrust.assign
// CHECK: emitrust.global_store %{{.*}}, @gx : i32
// CHECK: return
void scalar_write(int v) {
  int *s = &gx;
  *s = v;
}

int main(void) { return 0; }
