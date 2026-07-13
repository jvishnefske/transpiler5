// FR-1: Dialect registration and round-trip: emitrust-opt parses and re-prints every MVP op with no loss.
// RUN: emitrust-opt %s | emitrust-opt | FileCheck %s

// CHECK: emitrust.use "std::vec::Vec"
emitrust.use "std::vec::Vec"

// CHECK: emitrust.verbatim "// raw text"
emitrust.verbatim "// raw text"

// CHECK-LABEL: emitrust.func @signature(
// CHECK-SAME: i32, {{.*}}: i32) -> i32
emitrust.func @signature(%arg0: i32, %arg1: i32) -> i32 {
  // CHECK: emitrust.return %{{.*}} : i32
  emitrust.return %arg0 : i32
}

// CHECK-LABEL: emitrust.func @no_result
emitrust.func @no_result() {
  // CHECK: emitrust.return
  emitrust.return
}

// CHECK-LABEL: emitrust.func @constants
emitrust.func @constants() {
  // CHECK: emitrust.constant <42 : i32> : i32
  %0 = emitrust.constant <42 : i32> : i32
  // CHECK: emitrust.constant <4.2{{[0-9e+.]*}} : f64> : f64
  %1 = emitrust.constant <4.2 : f64> : f64
  // CHECK: emitrust.constant <true> : i1
  %2 = emitrust.constant <true> : i1
  // CHECK: emitrust.constant <#emitrust.opaque<"Vec::new()">> : !emitrust.opaque<"Vec<i32>">
  %3 = emitrust.constant <#emitrust.opaque<"Vec::new()">> : !emitrust.opaque<"Vec<i32>">
  emitrust.return
}

// CHECK-LABEL: emitrust.func @literal
emitrust.func @literal() {
  // CHECK: emitrust.literal "x.len()" : index
  %0 = emitrust.literal "x.len()" : index
  emitrust.return
}

// CHECK-LABEL: emitrust.func @lets_and_assign
emitrust.func @lets_and_assign(%arg0: i32) {
  // CHECK: emitrust.let %{{.*}} : i32
  %0 = emitrust.let %arg0 : i32
  // CHECK: emitrust.let mut %{{.*}} : i32
  %1 = emitrust.let mut %arg0 : i32
  // CHECK: emitrust.assign %{{.*}} = %{{.*}} : i32
  emitrust.assign %1 = %0 : i32
  emitrust.return
}

// CHECK-LABEL: emitrust.func @binops
emitrust.func @binops(%arg0: i32, %arg1: i32) {
  // CHECK: emitrust.add %{{.*}}, %{{.*}} : i32
  %0 = emitrust.add %arg0, %arg1 : i32
  // CHECK: emitrust.sub %{{.*}}, %{{.*}} : i32
  %1 = emitrust.sub %arg0, %arg1 : i32
  // CHECK: emitrust.mul %{{.*}}, %{{.*}} : i32
  %2 = emitrust.mul %arg0, %arg1 : i32
  // CHECK: emitrust.div %{{.*}}, %{{.*}} : i32
  %3 = emitrust.div %arg0, %arg1 : i32
  // CHECK: emitrust.rem %{{.*}}, %{{.*}} : i32
  %4 = emitrust.rem %arg0, %arg1 : i32
  emitrust.return
}

// CHECK-LABEL: emitrust.func @compares
emitrust.func @compares(%arg0: i32, %arg1: i32) {
  // CHECK: emitrust.cmp eq, %{{.*}}, %{{.*}} : (i32, i32) -> i1
  %0 = emitrust.cmp eq, %arg0, %arg1 : (i32, i32) -> i1
  // CHECK: emitrust.cmp ne, %{{.*}}, %{{.*}} : (i32, i32) -> i1
  %1 = emitrust.cmp ne, %arg0, %arg1 : (i32, i32) -> i1
  // CHECK: emitrust.cmp lt, %{{.*}}, %{{.*}} : (i32, i32) -> i1
  %2 = emitrust.cmp lt, %arg0, %arg1 : (i32, i32) -> i1
  // CHECK: emitrust.cmp le, %{{.*}}, %{{.*}} : (i32, i32) -> i1
  %3 = emitrust.cmp le, %arg0, %arg1 : (i32, i32) -> i1
  // CHECK: emitrust.cmp gt, %{{.*}}, %{{.*}} : (i32, i32) -> i1
  %4 = emitrust.cmp gt, %arg0, %arg1 : (i32, i32) -> i1
  // CHECK: emitrust.cmp ge, %{{.*}}, %{{.*}} : (i32, i32) -> i1
  %5 = emitrust.cmp ge, %arg0, %arg1 : (i32, i32) -> i1
  emitrust.return
}

// CHECK-LABEL: emitrust.func @casts
emitrust.func @casts(%arg0: i32) {
  // CHECK: emitrust.cast %{{.*}} : i32 to i64
  %0 = emitrust.cast %arg0 : i32 to i64
  emitrust.return
}

// CHECK-LABEL: emitrust.func @calls
emitrust.func @calls(%arg0: i32, %arg1: i32) {
  // CHECK: emitrust.call_opaque "consume"(%{{.*}}) : (i32) -> ()
  emitrust.call_opaque "consume"(%arg0) : (i32) -> ()
  // CHECK: emitrust.call_opaque "foo"(%{{.*}}, %{{.*}}) : (i32, i32) -> i32
  %0 = emitrust.call_opaque "foo"(%arg0, %arg1) : (i32, i32) -> i32
  // CHECK: emitrust.call_opaque "pair"(%{{.*}}) : (i32) -> (i32, i64)
  %1:2 = emitrust.call_opaque "pair"(%arg0) : (i32) -> (i32, i64)
  emitrust.return
}

// CHECK-LABEL: emitrust.func @conditionals
emitrust.func @conditionals(%arg0: i1) {
  // CHECK: emitrust.if %{{.*}} {
  emitrust.if %arg0 {
    %0 = emitrust.constant <1 : i32> : i32
  }
  // CHECK: emitrust.if %{{.*}} {
  // CHECK: } else {
  emitrust.if %arg0 {
    %1 = emitrust.constant <2 : i32> : i32
  } else {
    %2 = emitrust.constant <3 : i32> : i32
  }
  emitrust.return
}

// CHECK-LABEL: emitrust.func @loops
emitrust.func @loops(%arg0: index, %arg1: index, %arg2: index) {
  // CHECK: emitrust.for %{{.*}} = %{{.*}} to %{{.*}} step %{{.*}} {
  emitrust.for %i = %arg0 to %arg1 step %arg2 {
    %0 = emitrust.let %i : index
  }
  emitrust.return
}

// CHECK-LABEL: emitrust.func @typed_loop
emitrust.func @typed_loop(%arg0: i32, %arg1: i32, %arg2: i32) {
  // CHECK: emitrust.for %{{.*}} = %{{.*}} to %{{.*}} step %{{.*}} : i32 {
  emitrust.for %i = %arg0 to %arg1 step %arg2 : i32 {
    %0 = emitrust.add %i, %i : i32
  }
  emitrust.return
}

// CHECK-LABEL: emitrust.func @loop_jumps
emitrust.func @loop_jumps(%arg0: i1) {
  // CHECK: emitrust.loop {
  emitrust.loop {
    // CHECK: emitrust.if %{{.*}} {
    emitrust.if %arg0 {
      // CHECK: emitrust.break
      emitrust.break
    }
    // CHECK: emitrust.continue
    emitrust.continue
  }
  emitrust.return
}

// CHECK: emitrust.struct_def @Point ["x", "y"] [i32, i32]
emitrust.struct_def @Point ["x", "y"] [i32, i32]

// CHECK-LABEL: emitrust.func @variables
emitrust.func @variables() {
  // CHECK: emitrust.variable <42 : i32> : !emitrust.lvalue<i32>
  %0 = emitrust.variable <42 : i32> : !emitrust.lvalue<i32>
  // CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  %1 = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  emitrust.return
}

// CHECK-LABEL: emitrust.func @places
emitrust.func @places(%arg0: !emitrust.mut_ref<i32>, %arg1: index, %arg2: i32) {
  %p = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Point">>
  // CHECK: emitrust.member %{{.*}}["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %x = emitrust.member %p["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %a = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  // CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, index) -> !emitrust.lvalue<i32>
  %e = emitrust.subscript %a[%arg1] : (!emitrust.lvalue<!emitrust.array<4xi32>>, index) -> !emitrust.lvalue<i32>
  // CHECK: emitrust.deref %{{.*}} : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
  %d = emitrust.deref %arg0 : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
  // CHECK: emitrust.load %{{.*}} : (!emitrust.lvalue<i32>) -> i32
  %l = emitrust.load %x : (!emitrust.lvalue<i32>) -> i32
  // CHECK: emitrust.addr_of %{{.*}} : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  %r = emitrust.addr_of %x : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  // CHECK: emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
  %m = emitrust.addr_of mut %x : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
  // CHECK: emitrust.assign %{{.*}} = %{{.*}} : !emitrust.lvalue<i32>
  emitrust.assign %e = %arg2 : !emitrust.lvalue<i32>
  emitrust.return
}

// CHECK-LABEL: emitrust.func @call_args
emitrust.func @call_args(%arg0: i32) {
  // CHECK: emitrust.call_opaque "print!"(%{{.*}}) {args = ["x={}\0A", 0 : index]} : (i32) -> ()
  emitrust.call_opaque "print!"(%arg0) {args = ["x={}\0A", 0 : index]} : (i32) -> ()
  emitrust.return
}
