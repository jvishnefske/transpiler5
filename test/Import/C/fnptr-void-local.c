// CTS-F (00210): a LOCAL `void *` holder initialized with the address of
// one known in-TU function (`&f` or the decayed `f`), never reassigned,
// whose EVERY value use is an explicit cast to exactly f's signature in
// callee position `((T (*)(...))fp)(...)`, imports as an ordinary local
// !emitrust.fn_ptr (the FR-29 model): the holder becomes a fn_ptr
// variable assigned the opaque Some(target) constant, each cast-call
// peels the cast into a plain emitrust.call_indirect, and no cast op
// survives in the function. Attributes inside the cast type — both 00210
// spellings — are discarded (clang already drops them on this target).
// Out-of-shape holders keep the existing rejection (see
// fnptr-void-local-invalid.c).
// RUN: emitrust-import-c %s | FileCheck %s

int forty_two(void) { return 42; }
int add_three(int x) { return x + 3; }

// The plain shape: `&f` init, a single cast-call value use.
int call_plain(void) {
  void *fp = &forty_two;
  return ((int (*)(void))fp)();
}
// CHECK-LABEL: func.func @call_plain
// CHECK: %[[FP:.*]] = emitrust.variable named "fp" : !emitrust.lvalue<!emitrust.fn_ptr<() -> i32>>
// CHECK: %[[T:.*]] = emitrust.constant <#emitrust.opaque<"Some(forty_two)">> : !emitrust.fn_ptr<() -> i32>
// CHECK: emitrust.assign %[[FP]] = %[[T]]
// CHECK: %[[V:.*]] = emitrust.load %[[FP]]
// CHECK: emitrust.call_indirect %[[V]]() : (!emitrust.fn_ptr<() -> i32>) -> i32
// CHECK-NOT: emitrust.cast

// The decayed init spelling (`= f`, no ampersand) and the two 00210
// attributed cast spellings through ONE holder: `((ATTR int (*)(void))fp)`
// and `((int (ATTR *)(void))fp)` both peel to the same call_indirect.
#define ATTR __attribute__((__noinline__))
int call_attr(void) {
  void *fp = forty_two;
  int a = ((ATTR int (*)(void))fp)();
  int b = ((int (ATTR *)(void))fp)();
  return a + b;
}
// CHECK-LABEL: func.func @call_attr
// CHECK: emitrust.variable named "fp" : !emitrust.lvalue<!emitrust.fn_ptr<() -> i32>>
// CHECK: emitrust.constant <#emitrust.opaque<"Some(forty_two)">> : !emitrust.fn_ptr<() -> i32>
// CHECK: emitrust.call_indirect %{{.*}}() : (!emitrust.fn_ptr<() -> i32>) -> i32
// CHECK: emitrust.call_indirect %{{.*}}() : (!emitrust.fn_ptr<() -> i32>) -> i32
// CHECK-NOT: emitrust.cast

// A target with a parameter: the cast spells exactly the target's
// signature and the argument flows through the indirect call.
int call_arg(int n) {
  void *fp = &add_three;
  return ((int (*)(int))fp)(n);
}
// CHECK-LABEL: func.func @call_arg
// CHECK: emitrust.variable named "fp" : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
// CHECK: emitrust.constant <#emitrust.opaque<"Some(add_three)">> : !emitrust.fn_ptr<(i32) -> i32>
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}) : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
// CHECK-NOT: emitrust.cast
