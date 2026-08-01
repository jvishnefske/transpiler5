// FR-29: function pointer rendering: !emitrust.fn_ptr renders as the
// nullable `Option<fn(...)>` (the C null pointer is None, so no unsafe
// sentinel exists), emitrust.call_indirect renders as a
// .expect("null function pointer") call, default values render as None,
// and fn_ptr struct fields and globals render like every other field type.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s --strict-whitespace

// CHECK:      fn types(v0: Option<fn(i32, i32) -> i32>, v1: Option<fn()>, v2: Option<fn() -> i32>, v3: Option<fn(Option<fn(i32) -> i32>) -> Option<fn() -> i32>>) {
emitrust.func @types(%arg0: !emitrust.fn_ptr<(i32, i32) -> i32>,
                     %arg1: !emitrust.fn_ptr<()>,
                     %arg2: !emitrust.fn_ptr<() -> i32>,
                     %arg3: !emitrust.fn_ptr<(!emitrust.fn_ptr<(i32) -> i32>)
                                             -> !emitrust.fn_ptr<() -> i32>>) {
  // CHECK-NEXT:    let v4: Option<fn(i32, i32) -> i32> = v0;
  %0 = emitrust.let %arg0 : !emitrust.fn_ptr<(i32, i32) -> i32>
  emitrust.return
}

// CHECK:      fn calls(v0: Option<fn(i32, i32) -> i32>, v1: Option<fn()>, v2: i32) -> i32 {
emitrust.func @calls(%arg0: !emitrust.fn_ptr<(i32, i32) -> i32>,
                     %arg1: !emitrust.fn_ptr<()>, %arg2: i32) -> i32 {
  // CHECK-NEXT:    let v3: i32 = v0.expect("null function pointer")(v2, v2);
  %0 = emitrust.call_indirect %arg0(%arg2, %arg2)
      : (!emitrust.fn_ptr<(i32, i32) -> i32>, i32, i32) -> i32
  // CHECK-NEXT:    v1.expect("null function pointer")();
  emitrust.call_indirect %arg1() : (!emitrust.fn_ptr<()>) -> ()
  // CHECK-NEXT:    return v3;
  emitrust.return %0 : i32
}

// CHECK:      fn defaults() {
emitrust.func @defaults() {
  // CHECK-NEXT:    let mut v0: Option<fn(i32) -> i32> = None;
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  // CHECK-NEXT:    let v1: Option<fn(i32) -> i32> = Some(add);
  %1 = emitrust.constant <#emitrust.opaque<"Some(add)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  // CHECK-NEXT:    v0 = v1;
  emitrust.assign %0 = %1 : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  // CHECK-NEXT:    let v2: Option<fn(i32) -> i32> = None;
  %2 = emitrust.constant <#emitrust.opaque<"None">>
      : !emitrust.fn_ptr<(i32) -> i32>
  // A `!= None` comparison lowers to an Option null-test to avoid
  // `unpredictable_function_pointer_comparisons`.
  // A `!= None` comparison lowers to an Option null-test to avoid
  // `unpredictable_function_pointer_comparisons`.
  // CHECK-NEXT:    let v3: bool = v1.is_some();
  %3 = emitrust.cmp ne, %1, %2
      : (!emitrust.fn_ptr<(i32) -> i32>, !emitrust.fn_ptr<(i32) -> i32>) -> i1
  // Two non-null function pointers compare by address, None-aware.
  // CHECK-NEXT:    let v4: bool = match (v1, v1) { (Some(l), Some(r)) => core::ptr::fn_addr_eq(l, r), (None, None) => true, _ => false };
  %4 = emitrust.cmp eq, %1, %1
      : (!emitrust.fn_ptr<(i32) -> i32>, !emitrust.fn_ptr<(i32) -> i32>) -> i1
  emitrust.return
}

// CHECK:      #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct Dispatch {
// CHECK-NEXT:     op: Option<fn(i32, i32) -> i32>,
// CHECK-NEXT: }
emitrust.struct_def @Dispatch ["op"] [!emitrust.fn_ptr<(i32, i32) -> i32>]

// CHECK:      thread_local! {
// CHECK-NEXT:     static handler: std::cell::Cell<Option<fn(i32) -> i32>> = std::cell::Cell::new(None);
// CHECK-NEXT: }
emitrust.global @handler : !emitrust.fn_ptr<(i32) -> i32>

// CHECK:      thread_local! {
// CHECK-NEXT:     static bound: std::cell::Cell<Option<fn(i32, i32) -> i32>> = std::cell::Cell::new(Some(add));
// CHECK-NEXT: }
emitrust.global @bound <#emitrust.opaque<"Some(add)">>
    : !emitrust.fn_ptr<(i32, i32) -> i32>
