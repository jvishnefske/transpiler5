// RUN: emitrust-import-c %s | FileCheck %s

struct Pair {
  int a;
  int b;
};

void bump(int *v) {
  *v = *v + 1;
}

void fill(struct Pair *p) {
  p->a = 1;
  p->b = 2;
}

int drive(void) {
  int x = 41;
  bump(&x);
  struct Pair pr;
  fill(&pr);
  return x;
}

// CHECK: emitrust.struct_def @Pair ["a", "b"] [i32, i32]

// An int* parameter arrives as !emitrust.mut_ref<i32>; *v reads and writes
// go through deref places.
// CHECK-LABEL: func.func @bump
// CHECK-SAME: (%[[V:.*]]: !emitrust.mut_ref<i32>)
// CHECK: %[[PLACE:.*]] = emitrust.deref %[[V]] : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load %{{.*}} : (!emitrust.lvalue<i32>) -> i32
// CHECK: arith.addi
// CHECK: emitrust.assign %[[PLACE]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: return

// CHECK-LABEL: func.func @fill
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"Pair">>)
// CHECK: emitrust.deref
// CHECK: emitrust.member %{{.*}}["a"]
// CHECK: emitrust.assign
// CHECK: emitrust.member %{{.*}}["b"]
// CHECK: emitrust.assign

// An address-taken scalar local stays an EmitRust variable (never promoted),
// and &x / &pr produce mutable references passed to the callees.
// CHECK-LABEL: func.func @drive
// CHECK: %[[X:.*]] = emitrust.variable named "x" : !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[X]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[XREF:.*]] = emitrust.addr_of mut %[[X]] : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
// CHECK: call @bump(%[[XREF]])
// CHECK: %[[PR:.*]] = emitrust.variable named "pr" : !emitrust.lvalue<!emitrust.struct<"Pair">>
// CHECK: %[[PREF:.*]] = emitrust.addr_of mut %[[PR]] : (!emitrust.lvalue<!emitrust.struct<"Pair">>) -> !emitrust.mut_ref<!emitrust.struct<"Pair">>
// CHECK: call @fill(%[[PREF]])
// CHECK: emitrust.load %[[X]] : (!emitrust.lvalue<i32>) -> i32
// CHECK: return
