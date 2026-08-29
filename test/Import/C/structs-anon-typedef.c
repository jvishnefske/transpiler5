// C99-6: `typedef struct { ... } T;` gives the tagless record its typedef
// name; the struct imports, and its fields read/write and pass to functions
// exactly like a tagged struct. Across translation units the typedef name is
// the dedup key: an identical shape imports once. A bare anonymous struct
// with no typedef name gets a synthesized `Anon<hash>` name instead, the
// uppercase hex content hash of its field shape (CTS-R1/FR-151; see
// structs-anon-bare.c). The hash is never spelled out in a test -- it is
// captured into a FileCheck variable -- so extending the shape key cannot
// churn this golden.
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/valid.c | FileCheck %s
// RUN: emitrust-import-c %t/tu-a.c %t/tu-b.c | FileCheck %s --check-prefix=DEDUP
// RUN: emitrust-import-c %t/bare.c | FileCheck %s --check-prefix=BARE

//--- valid.c
typedef struct {
  int x;
  int y;
} Point;

void set_origin(Point *p) {
  p->x = 0;
  p->y = 0;
}

int manhattan(Point q) { return q.x + q.y; }

int use_point(void) {
  Point pt;
  pt.x = 3;
  pt.y = 4;
  set_origin(&pt);
  return manhattan(pt) + pt.x;
}

// The typedef name is the struct's name, same as a tagged struct.
// CHECK: emitrust.struct_def @Point ["x", "y"] [i32, i32]

// Struct pointer parameter: deref + member + assign through the typedef name.
// CHECK-LABEL: func.func @set_origin
// CHECK-SAME: (%[[P:.*]]: !emitrust.mut_ref<!emitrust.struct<"Point">>)
// CHECK: %[[PL:.*]] = emitrust.deref %[[P]] : (!emitrust.mut_ref<!emitrust.struct<"Point">>) -> !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK: emitrust.member %[[PL]]["x"]
// CHECK: emitrust.assign

// By-value struct parameter, field reads.
// CHECK-LABEL: func.func @manhattan
// CHECK-SAME: (%{{.*}}: !emitrust.struct<"Point">) -> i32
// CHECK: emitrust.member %{{.*}}["x"]
// CHECK: emitrust.load
// CHECK: arith.addi

// Local, field writes, address-of argument, by-value argument.
// CHECK-LABEL: func.func @use_point
// CHECK: %[[PT:.*]] = emitrust.variable named "pt" : !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK: emitrust.member %[[PT]]["x"]
// CHECK: emitrust.assign
// CHECK: emitrust.addr_of mut %[[PT]]
// CHECK: call @set_origin
// CHECK: call @manhattan

//--- tu-a.c
typedef struct {
  int lo;
  int hi;
} Range;

int width(Range r) { return r.hi - r.lo; }

//--- tu-b.c
typedef struct {
  int lo;
  int hi;
} Range;

int width(Range r);

int mid(Range r) { return r.lo + width(r) / 2; }

// The identical shape reached through the same typedef name in two TUs
// imports exactly once.
// DEDUP: emitrust.struct_def @Range ["lo", "hi"] [i32, i32]
// DEDUP-NOT: emitrust.struct_def @Range
// DEDUP-DAG: func.func @width
// DEDUP-DAG: func.func @mid

//--- bare.c
struct {
  int x;
} g;

// A bare anonymous struct imports under a synthesized shape-keyed name.
// BARE: emitrust.struct_def @[[BAREX:Anon[0-9A-F]+]] ["x"] [i32]
// BARE: emitrust.global @g : !emitrust.struct<"[[BAREX]]">
