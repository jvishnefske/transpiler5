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

// CHECK-LABEL: emitrust.func @bitops
emitrust.func @bitops(%arg0: i32, %arg1: i32) {
  // CHECK: emitrust.and %{{.*}}, %{{.*}} : i32
  %0 = emitrust.and %arg0, %arg1 : i32
  // CHECK: emitrust.or %{{.*}}, %{{.*}} : i32
  %1 = emitrust.or %arg0, %arg1 : i32
  // CHECK: emitrust.xor %{{.*}}, %{{.*}} : i32
  %2 = emitrust.xor %arg0, %arg1 : i32
  // CHECK: emitrust.shl %{{.*}}, %{{.*}} : i32
  %3 = emitrust.shl %arg0, %arg1 : i32
  // CHECK: emitrust.shr %{{.*}}, %{{.*}} : i32
  %4 = emitrust.shr %arg0, %arg1 : i32
  emitrust.return
}

// The binary operations round-trip on unsigned integer types as well; the
// emitter renders these as u8..u64 with C's wrap-around semantics.
// CHECK-LABEL: emitrust.func @unsigned_binops
emitrust.func @unsigned_binops(%arg0: ui32, %arg1: ui32) {
  // CHECK: emitrust.add %{{.*}}, %{{.*}} : ui32
  %0 = emitrust.add %arg0, %arg1 : ui32
  // CHECK: emitrust.div %{{.*}}, %{{.*}} : ui32
  %1 = emitrust.div %arg0, %arg1 : ui32
  // CHECK: emitrust.shr %{{.*}}, %{{.*}} : ui32
  %2 = emitrust.shr %arg0, %arg1 : ui32
  // CHECK: emitrust.cmp lt, %{{.*}}, %{{.*}} : (ui32, ui32) -> i1
  %3 = emitrust.cmp lt, %arg0, %arg1 : (ui32, ui32) -> i1
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

// CHECK-LABEL: emitrust.func @selects
emitrust.func @selects(%arg0: i1, %arg1: i32, %arg2: i32, %arg3: f64,
                       %arg4: f64) {
  // CHECK: emitrust.select %{{.*}}, %{{.*}}, %{{.*}} : i32
  %0 = emitrust.select %arg0, %arg1, %arg2 : i32
  // CHECK: emitrust.select %{{.*}}, %{{.*}}, %{{.*}} : f64
  %1 = emitrust.select %arg0, %arg3, %arg4 : f64
  // A select of selects: the result feeds another select's value operand.
  // CHECK: emitrust.select %{{.*}}, %{{.*}}, %{{.*}} : i32
  %2 = emitrust.select %arg0, %0, %arg1 : i32
  emitrust.return
}

// Dialect-typed select values round-trip with the fully qualified type.
// CHECK-LABEL: emitrust.func @select_opaque
emitrust.func @select_opaque(%arg0: i1, %arg1: !emitrust.opaque<"Wrapping<i32>">,
                             %arg2: !emitrust.opaque<"Wrapping<i32>">) {
  // CHECK: emitrust.select %{{.*}}, %{{.*}}, %{{.*}} : !emitrust.opaque<"Wrapping<i32>">
  %0 = emitrust.select %arg0, %arg1, %arg2 : !emitrust.opaque<"Wrapping<i32>">
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

// A field-less struct_def (C's `struct T {};`) round-trips with empty arrays.
// CHECK: emitrust.struct_def @Empty [] []
emitrust.struct_def @Empty [] []

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

// CHECK-LABEL: emitrust.func @switches
emitrust.func @switches(%arg0: i32, %arg1: index) {
  // CHECK:      emitrust.switch %{{.*}} : i32
  // CHECK-NEXT: case 0 {
  // CHECK:      case -4 {
  // CHECK:      default {
  emitrust.switch %arg0 : i32
  case 0 {
    %0 = emitrust.constant <1 : i32> : i32
  }
  case -4 {
    %1 = emitrust.constant <2 : i32> : i32
  }
  default {
  }
  // The single-case form produced for lifted loop-exit dispatch.
  // CHECK:      emitrust.switch %{{.*}} : index
  // CHECK-NEXT: case 1 {
  // CHECK:      default {
  emitrust.switch %arg1 : index
  case 1 {
  }
  default {
  }
  emitrust.return
}

// A break/continue under a switch under a loop is legal: the switch is
// transparent for the loop-jump verifier walk.
// CHECK-LABEL: emitrust.func @switch_in_loop
emitrust.func @switch_in_loop(%arg0: i32) {
  // CHECK: emitrust.loop {
  emitrust.loop {
    // CHECK: emitrust.switch %{{.*}} : i32
    emitrust.switch %arg0 : i32
    case 0 {
      // CHECK: emitrust.break
      emitrust.break
    }
    default {
      // CHECK: emitrust.continue
      emitrust.continue
    }
  }
  emitrust.return
}

// CHECK: emitrust.enum_def @Color ["Red", "Green", "Blue"] [0, 1, 2]
emitrust.enum_def @Color ["Red", "Green", "Blue"] [0, 1, 2]

// CHECK-LABEL: emitrust.func @enums
emitrust.func @enums(%arg0: !emitrust.enum<"Color">, %arg1: !emitrust.enum<"Color">) {
  // CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.enum<"Color">>
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.enum<"Color">>
  // CHECK: emitrust.constant <#emitrust.opaque<"Color::Red">> : !emitrust.enum<"Color">
  %1 = emitrust.constant <#emitrust.opaque<"Color::Red">> : !emitrust.enum<"Color">
  // CHECK: emitrust.cmp eq, %{{.*}}, %{{.*}} : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1
  %2 = emitrust.cmp eq, %arg0, %arg1 : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1
  // CHECK: emitrust.cmp ne, %{{.*}}, %{{.*}} : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1
  %3 = emitrust.cmp ne, %arg0, %arg1 : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1
  // CHECK: emitrust.cast %{{.*}} : !emitrust.enum<"Color"> to i32
  %4 = emitrust.cast %arg0 : !emitrust.enum<"Color"> to i32
  // CHECK: emitrust.select %{{.*}}, %{{.*}}, %{{.*}} : !emitrust.enum<"Color">
  %5 = emitrust.select %2, %arg0, %arg1 : !emitrust.enum<"Color">
  emitrust.return
}

// CHECK: emitrust.enum_def @Mode ["Off", "On"] [0, 1] {unsigned_underlying}
emitrust.enum_def @Mode ["Off", "On"] [0, 1] {unsigned_underlying}

// CHECK-LABEL: emitrust.func @enum_open
emitrust.func @enum_open(%arg0: i32) {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.enum<"Mode">>
  // CHECK: emitrust.cast %{{.*}} : i32 to !emitrust.enum<"Mode">
  %1 = emitrust.cast %arg0 : i32 to !emitrust.enum<"Mode">
  emitrust.assign %0 = %1 : !emitrust.lvalue<!emitrust.enum<"Mode">>
  // CHECK: emitrust.enum_raw %{{.*}} : (!emitrust.lvalue<!emitrust.enum<"Mode">>) -> !emitrust.lvalue<ui32>
  %2 = emitrust.enum_raw %0 : (!emitrust.lvalue<!emitrust.enum<"Mode">>) -> !emitrust.lvalue<ui32>
  // CHECK: emitrust.addr_of mut %{{.*}} : (!emitrust.lvalue<ui32>) -> !emitrust.mut_ref<ui32>
  %3 = emitrust.addr_of mut %2 : (!emitrust.lvalue<ui32>) -> !emitrust.mut_ref<ui32>
  emitrust.return
}

// CHECK: emitrust.global @counter <0 : i32> : i32
emitrust.global @counter <0 : i32> : i32

// CHECK: emitrust.global const @limit <100 : i32> : i32
emitrust.global const @limit <100 : i32> : i32

// CHECK: emitrust.global @ratio <2.5{{[0-9e+.]*}} : f64> : f64
emitrust.global @ratio <2.5 : f64> : f64

// CHECK: emitrust.global @flag <true> : i1
emitrust.global @flag <true> : i1

// CHECK: emitrust.global @table : !emitrust.array<4xi32>
emitrust.global @table : !emitrust.array<4xi32>

// CHECK: emitrust.global const @fixed : !emitrust.array<2xf64>
emitrust.global const @fixed : !emitrust.array<2xf64>

// CHECK: emitrust.global @origin : !emitrust.struct<"Point">
emitrust.global @origin : !emitrust.struct<"Point">

// An aggregate initializer is an ArrayAttr with one entry per element.
// CHECK: emitrust.global const @pair <[1 : i32, -2 : i32]> : !emitrust.array<2xi32>
emitrust.global const @pair <[1 : i32, -2 : i32]> : !emitrust.array<2xi32>

// A struct initializer lists one entry per field, in declaration order.
// CHECK: emitrust.global @unit <[3 : i32, -4 : i32]> : !emitrust.struct<"Point">
emitrust.global @unit <[3 : i32, -4 : i32]> : !emitrust.struct<"Point">

// Aggregate elements nest: an array of structs takes nested lists.
// CHECK: emitrust.global @corners <{{\[}}[1 : i32, 2 : i32], [3 : i32, 4 : i32]]> : !emitrust.array<2x!emitrust.struct<"Point">>
emitrust.global @corners <[[1 : i32, 2 : i32], [3 : i32, 4 : i32]]> : !emitrust.array<2x!emitrust.struct<"Point">>

// CHECK-LABEL: emitrust.func @global_access
emitrust.func @global_access() {
  // CHECK: emitrust.global_load @counter : i32
  %0 = emitrust.global_load @counter : i32
  // CHECK: emitrust.global_store %{{.*}}, @counter : i32
  emitrust.global_store %0, @counter : i32
  // CHECK: emitrust.global_load @limit : i32
  %1 = emitrust.global_load @limit : i32
  // CHECK: emitrust.global_load @table : !emitrust.array<4xi32>
  %2 = emitrust.global_load @table : !emitrust.array<4xi32>
  // CHECK: emitrust.global_store %{{.*}}, @table : !emitrust.array<4xi32>
  emitrust.global_store %2, @table : !emitrust.array<4xi32>
  emitrust.return
}

// CHECK-LABEL: emitrust.func @slice_places
emitrust.func @slice_places(%arg0: !emitrust.mut_ref<!emitrust.slice<i32>>, %arg1: i64, %arg2: index) {
  // CHECK: emitrust.deref %{{.*}} : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
  %s = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.slice<i32>>) -> !emitrust.lvalue<!emitrust.slice<i32>>
  // A subscript accepts a slice-typed lvalue base.
  // CHECK: emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.lvalue<i32>
  %e = emitrust.subscript %s[%arg1] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.lvalue<i32>
  %a = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
  // CHECK: emitrust.slice_of mut %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  %m = emitrust.slice_of mut %a[%arg1] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  // CHECK: emitrust.slice_of %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi32>>, index) -> !emitrust.ref<!emitrust.slice<i32>>
  %r = emitrust.slice_of %a[%arg2] : (!emitrust.lvalue<!emitrust.array<4xi32>>, index) -> !emitrust.ref<!emitrust.slice<i32>>
  // Reslicing a deref'd slice place composes.
  // CHECK: emitrust.slice_of mut %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  %t = emitrust.slice_of mut %s[%arg1] : (!emitrust.lvalue<!emitrust.slice<i32>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i32>>
  emitrust.return
}

// CHECK-LABEL: emitrust.func @fn_ptr_ops(
emitrust.func @fn_ptr_ops(%arg0: !emitrust.fn_ptr<(i32, i32) -> i32>,
                          %arg1: !emitrust.fn_ptr<()>, %arg2: i32) {
  // CHECK: emitrust.constant <#emitrust.opaque<"Some(add)">> : !emitrust.fn_ptr<(i32, i32) -> i32>
  %0 = emitrust.constant <#emitrust.opaque<"Some(add)">> : !emitrust.fn_ptr<(i32, i32) -> i32>
  // CHECK: emitrust.constant <#emitrust.opaque<"None">> : !emitrust.fn_ptr<(i32, i32) -> i32>
  %1 = emitrust.constant <#emitrust.opaque<"None">> : !emitrust.fn_ptr<(i32, i32) -> i32>
  // CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}, %{{.*}}) : (!emitrust.fn_ptr<(i32, i32) -> i32>, i32, i32) -> i32
  %2 = emitrust.call_indirect %arg0(%arg2, %arg2)
      : (!emitrust.fn_ptr<(i32, i32) -> i32>, i32, i32) -> i32
  // CHECK: emitrust.call_indirect %{{.*}}() : (!emitrust.fn_ptr<()>) -> ()
  emitrust.call_indirect %arg1() : (!emitrust.fn_ptr<()>) -> ()
  // CHECK: emitrust.cmp eq, %{{.*}}, %{{.*}} : (!emitrust.fn_ptr<(i32, i32) -> i32>, !emitrust.fn_ptr<(i32, i32) -> i32>) -> i1
  %3 = emitrust.cmp eq, %arg0, %1
      : (!emitrust.fn_ptr<(i32, i32) -> i32>, !emitrust.fn_ptr<(i32, i32) -> i32>) -> i1
  // CHECK: emitrust.cmp ne, %{{.*}}, %{{.*}} : (!emitrust.fn_ptr<(i32, i32) -> i32>, !emitrust.fn_ptr<(i32, i32) -> i32>) -> i1
  %4 = emitrust.cmp ne, %arg0, %0
      : (!emitrust.fn_ptr<(i32, i32) -> i32>, !emitrust.fn_ptr<(i32, i32) -> i32>) -> i1
  // CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.fn_ptr<(i32, i32) -> i32>>
  %5 = emitrust.variable : !emitrust.lvalue<!emitrust.fn_ptr<(i32, i32) -> i32>>
  // CHECK: emitrust.assign %{{.*}} = %{{.*}} : !emitrust.lvalue<!emitrust.fn_ptr<(i32, i32) -> i32>>
  emitrust.assign %5 = %0 : !emitrust.lvalue<!emitrust.fn_ptr<(i32, i32) -> i32>>
  emitrust.return
}

// CHECK: emitrust.struct_def @Dispatch ["op"] [!emitrust.fn_ptr<(i32, i32) -> i32>]
emitrust.struct_def @Dispatch ["op"] [!emitrust.fn_ptr<(i32, i32) -> i32>]

// CHECK: emitrust.global @handler : !emitrust.fn_ptr<(i32) -> i32>
emitrust.global @handler : !emitrust.fn_ptr<(i32) -> i32>

// CHECK: emitrust.global @bound <#emitrust.opaque<"Some(add)">> : !emitrust.fn_ptr<(i32, i32) -> i32>
emitrust.global @bound <#emitrust.opaque<"Some(add)">> : !emitrust.fn_ptr<(i32, i32) -> i32>

// CHECK: emitrust.struct_def @Owner_main_arr ["data"] [!emitrust.array<8xi32>]
emitrust.struct_def @Owner_main_arr ["data"] [!emitrust.array<8xi32>]

// CHECK: emitrust.impl "Owner_main_arr" {
emitrust.impl "Owner_main_arr" {
  // CHECK: emitrust.func @get(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %{{.*}}: i64) -> i32
  emitrust.func @get(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %arg1: i64) -> i32 {
    %s = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
    %d = emitrust.member %s["data"] : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>) -> !emitrust.lvalue<!emitrust.array<8xi32>>
    %e = emitrust.subscript %d[%arg1] : (!emitrust.lvalue<!emitrust.array<8xi32>>, i64) -> !emitrust.lvalue<i32>
    %v = emitrust.load %e : (!emitrust.lvalue<i32>) -> i32
    // A sibling method call through the dereferenced receiver.
    // CHECK: emitrust.method_call %{{.*}}["touch"] (%{{.*}}) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> ()
    emitrust.method_call %s["touch"] (%arg1) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> ()
    emitrust.return %v : i32
  }
  // CHECK: emitrust.func @touch(
  emitrust.func @touch(%arg0: !emitrust.mut_ref<!emitrust.struct<"Owner_main_arr">>, %arg1: i64) {
    emitrust.return
  }
}

// CHECK-LABEL: emitrust.func @method_calls(
emitrust.func @method_calls(%arg0: i64) -> i32 {
  %o = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>
  // CHECK: emitrust.method_call %{{.*}}["touch"] (%{{.*}}) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> ()
  emitrust.method_call %o["touch"] (%arg0) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> ()
  // CHECK: %{{.*}} = emitrust.method_call %{{.*}}["get"] (%{{.*}}) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> i32
  %r = emitrust.method_call %o["get"] (%arg0) : (!emitrust.lvalue<!emitrust.struct<"Owner_main_arr">>, i64) -> i32
  emitrust.return %r : i32
}
