// M2: memory and aggregate emission: struct definitions, variable defaults,
// field and array element reads/writes, dereferenced reads/writes through a
// mutable reference parameter, borrows, and literal call arguments.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK:      #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct Point {
// CHECK-NEXT:     x: i32,
// CHECK-NEXT:     y: i32,
// CHECK-NEXT: }
emitrust.struct_def @Point ["x", "y"] [i32, i32]

// A field-less struct_def (C's `struct T {};`) prints unit-like with an
// empty brace body; declaration and copy still work through the derives.
// CHECK:      #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct Empty {}
emitrust.struct_def @Empty [] []

// CHECK-LABEL: fn empty_struct_value() {
// CHECK-NEXT:    let v0: Empty = Empty::default();
// CHECK-NEXT:    let _v1: Empty = v0;
// CHECK-NEXT:    let _v2: Empty = Empty::default();
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @empty_struct_value() {
  %a = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Empty">>
  %v = emitrust.load %a : (!emitrust.lvalue<!emitrust.struct<"Empty">>) -> !emitrust.struct<"Empty">
  %b = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Empty">>
  emitrust.assign %b = %v : !emitrust.lvalue<!emitrust.struct<"Empty">>
  emitrust.return
}

// CHECK-LABEL: fn defaults() {
// CHECK-NEXT:    let _v0: i32 = 42;
// CHECK-NEXT:    let _v1: i32 = 0;
// CHECK-NEXT:    let _v2: bool = false;
// CHECK-NEXT:    let _v3: f64 = 0.0;
// CHECK-NEXT:    let _v4: Point = Point::default();
// CHECK-NEXT:    let _v5: [i32; 4] = [0; 4];
// CHECK-NEXT:    let _v6: [Point; 2] = [Point::default(); 2];
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @defaults() {
  %0 = emitrust.variable <42 : i32> : !emitrust.lvalue<i32>
  %1 = emitrust.variable : !emitrust.lvalue<i32>
  %2 = emitrust.variable : !emitrust.lvalue<i1>
  %3 = emitrust.variable : !emitrust.lvalue<f64>
  %4 = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Point">>
  %5 = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  %6 = emitrust.variable : !emitrust.lvalue<!emitrust.array<2x!emitrust.struct<"Point">>>
  emitrust.return
}

// CHECK-LABEL: fn fields(v0: i32) -> i32 {
// CHECK-NEXT:    let mut v1: Point = Point::default();
// CHECK-NEXT:    v1.x = v0;
// CHECK-NEXT:    let v2: i32 = v1.y;
// CHECK-NEXT:    return v2;
// CHECK-NEXT:  }
emitrust.func @fields(%arg0: i32) -> i32 {
  %p = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Point">>
  %x = emitrust.member %p["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  emitrust.assign %x = %arg0 : !emitrust.lvalue<i32>
  %y = emitrust.member %p["y"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %y : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// CHECK-LABEL: fn indexing(v0: i32, v1: usize, v2: i32) -> i32 {
// CHECK-NEXT:    let mut v3: [i32; 4] = [0; 4];
// CHECK-NEXT:    v3[v1] = v0;
// CHECK-NEXT:    v3[v2 as usize] = v0;
// CHECK-NEXT:    let v4: i32 = v3[v1];
// CHECK-NEXT:    return v4;
// CHECK-NEXT:  }
emitrust.func @indexing(%arg0: i32, %arg1: index, %arg2: i32) -> i32 {
  %a = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  %e = emitrust.subscript %a[%arg1] : (!emitrust.lvalue<!emitrust.array<4xi32>>, index) -> !emitrust.lvalue<i32>
  emitrust.assign %e = %arg0 : !emitrust.lvalue<i32>
  %f = emitrust.subscript %a[%arg2] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
  emitrust.assign %f = %arg0 : !emitrust.lvalue<i32>
  %r = emitrust.load %e : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// CHECK-LABEL: fn through_ref(v0: &mut i32, v1: i32) -> i32 {
// CHECK-NEXT:    *v0 = v1;
// CHECK-NEXT:    let v2: i32 = *v0;
// CHECK-NEXT:    return v2;
// CHECK-NEXT:  }
emitrust.func @through_ref(%arg0: !emitrust.mut_ref<i32>, %arg1: i32) -> i32 {
  %p = emitrust.deref %arg0 : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
  emitrust.assign %p = %arg1 : !emitrust.lvalue<i32>
  %r = emitrust.load %p : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// CHECK-LABEL: fn borrows() {
// CHECK-NEXT:    let mut v0: i32 = 1;
// CHECK-NEXT:    let _v1: &i32 = &v0;
// CHECK-NEXT:    let _v2: &mut i32 = &mut v0;
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @borrows() {
  %v = emitrust.variable <1 : i32> : !emitrust.lvalue<i32>
  %r = emitrust.addr_of %v : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  %m = emitrust.addr_of mut %v : (!emitrust.lvalue<i32>) -> !emitrust.mut_ref<i32>
  emitrust.return
}

// CHECK-LABEL: fn printing(v0: i32) {
// CHECK-NEXT:    print!("x={}\n", v0);
// CHECK-NEXT:    return;
// CHECK-NEXT:  }
emitrust.func @printing(%arg0: i32) {
  emitrust.call_opaque "print!"(%arg0) {args = ["x={}\0A", 0 : index]} : (i32) -> ()
  emitrust.return
}
