// RUN: emitrust-import-c %s | FileCheck %s

// FR-29: function pointers import as ordinary `!emitrust.fn_ptr` values
// (rendered `Option<fn(...)>`): function references become opaque
// `Some(name)` constants (signature-checked), NULL becomes `None`,
// indirect calls become `emitrust.call_indirect`, and truth tests compare
// against a `None` constant. Function pointers bypass the Phase-1a data
// pointer decomposition entirely: they live in `emitrust.variable` places
// like enums.

int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int zero(void) { return 0; }

// A local function pointer assigned both `&f` and plain `f`, then called.
int local_fn_ptr(void) {
  int (*fp)(int, int) = &add;
  fp = sub;
  return fp(5, 3);
}
// CHECK-LABEL: func.func @local_fn_ptr
// CHECK: %[[FP:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.fn_ptr<(i32, i32) -> i32>>
// CHECK: %[[ADDR:.*]] = emitrust.constant <#emitrust.opaque<"Some(add)">> : !emitrust.fn_ptr<(i32, i32) -> i32>
// CHECK: emitrust.assign %[[FP]] = %[[ADDR]]
// CHECK: %[[SUB:.*]] = emitrust.constant <#emitrust.opaque<"Some(sub)">> : !emitrust.fn_ptr<(i32, i32) -> i32>
// CHECK: emitrust.assign %[[FP]] = %[[SUB]]
// CHECK: %[[VAL:.*]] = emitrust.load %[[FP]]
// CHECK: emitrust.call_indirect %[[VAL]](%{{.*}}, %{{.*}}) : (!emitrust.fn_ptr<(i32, i32) -> i32>, i32, i32) -> i32

// The 00087 shape: a struct member function pointer set and called through
// the member place.
struct S {
  int (*op)(int, int);
};

int member_fn_ptr(void) {
  struct S v;
  v.op = add;
  return v.op(10, 20);
}
// CHECK-LABEL: func.func @member_fn_ptr
// CHECK: %[[V:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: %[[M:.*]] = emitrust.member %[[V]]["op"]
// CHECK: emitrust.assign %[[M]] = %{{.*}} : !emitrust.lvalue<!emitrust.fn_ptr<(i32, i32) -> i32>>
// CHECK: %[[M2:.*]] = emitrust.member %[[V]]["op"]
// CHECK: %[[MV:.*]] = emitrust.load %[[M2]]
// CHECK: emitrust.call_indirect %[[MV]](%{{.*}}, %{{.*}})

// The 00088 shape: a null-initialized global function pointer, truth
// tested, assigned, and called. The `(*gp)(...)` spelling cancels against
// the implicit decay and calls through the same pointer value.
int (*gp)(int, int) = 0;
// CHECK: emitrust.global @gp <#emitrust.opaque<"None">> : !emitrust.fn_ptr<(i32, i32) -> i32>

int global_fn_ptr(void) {
  if (gp)
    return gp(1, 2);
  gp = add;
  return (*gp)(3, 4);
}
// CHECK-LABEL: func.func @global_fn_ptr
// CHECK: %[[G:.*]] = emitrust.global_load @gp : !emitrust.fn_ptr<(i32, i32) -> i32>
// CHECK: %[[NONE:.*]] = emitrust.constant <#emitrust.opaque<"None">> : !emitrust.fn_ptr<(i32, i32) -> i32>
// CHECK: emitrust.cmp ne, %[[G]], %[[NONE]]
// CHECK: emitrust.global_store %{{.*}}, @gp : !emitrust.fn_ptr<(i32, i32) -> i32>
// CHECK: %[[G2:.*]] = emitrust.global_load @gp
// CHECK: emitrust.call_indirect %[[G2]](%{{.*}}, %{{.*}})

// A function pointer as a parameter (by value, not a reference) and the
// call through it.
int apply(int (*f)(int, int), int a, int b) { return f(a, b); }
// CHECK-LABEL: func.func @apply
// CHECK-SAME: (%{{.*}}: !emitrust.fn_ptr<(i32, i32) -> i32>, %{{.*}}: i32, %{{.*}}: i32) -> i32
// CHECK: emitrust.call_indirect

// A function pointer as a return value, produced from a function
// reference and from NULL.
int (*pick(int which))(int, int) {
  if (which)
    return add;
  return 0;
}
// CHECK-LABEL: func.func @pick
// CHECK-SAME: (%{{.*}}: i32) -> !emitrust.fn_ptr<(i32, i32) -> i32>
// CHECK: emitrust.constant <#emitrust.opaque<"Some(add)">>
// CHECK: emitrust.constant <#emitrust.opaque<"None">>

// A prototype-less K&R pointer maps to the zero-parameter form; the
// zero-argument call through it is checkable and allowed.
int noproto_fn_ptr(void) {
  int (*np)() = zero;
  return np();
}
// CHECK-LABEL: func.func @noproto_fn_ptr
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.fn_ptr<() -> i32>>
// CHECK: emitrust.constant <#emitrust.opaque<"Some(zero)">> : !emitrust.fn_ptr<() -> i32>
// CHECK: emitrust.call_indirect %{{.*}}() : (!emitrust.fn_ptr<() -> i32>) -> i32

// Equality against another function pointer and against NULL both map to
// emitrust.cmp on the fn_ptr type.
int compare_fn_ptr(void) {
  int (*fp)(int, int) = add;
  if (fp == sub)
    return 1;
  if (fp != 0)
    return 2;
  return 0;
}
// CHECK-LABEL: func.func @compare_fn_ptr
// CHECK: emitrust.cmp eq, %{{.*}}, %{{.*}} : (!emitrust.fn_ptr<(i32, i32) -> i32>, !emitrust.fn_ptr<(i32, i32) -> i32>) -> i1
// CHECK: emitrust.cmp ne, %{{.*}}, %{{.*}} : (!emitrust.fn_ptr<(i32, i32) -> i32>, !emitrust.fn_ptr<(i32, i32) -> i32>) -> i1

// A fn_ptr struct field in a file-scope initializer (CTS-L3, the 00089
// shape): the constant evaluator yields the target declaration (or the
// null constant), carried as the same opaque `Some(name)` / `None` forms
// a top-level fn_ptr global uses — signature-checked identically. The
// zero-filled tail of `= { add }` defaults the second field to `None`.
struct Ops {
  int (*bin)(int, int);
  int (*nil)(void);
  int tag;
};
struct Ops ops = {add, 0, 5};
struct Ops zeroed = {add};
// CHECK: emitrust.global @ops <[#emitrust.opaque<"Some(add)">, #emitrust.opaque<"None">, 5 : i32]> : !emitrust.struct<"Ops">
// CHECK: emitrust.global @zeroed <[#emitrust.opaque<"Some(add)">, #emitrust.opaque<"None">, 0 : i32]> : !emitrust.struct<"Ops">

// Calling through the field loads the staged struct copy and
// call_indirects its member.
// CHECK-LABEL: func.func @call_field
// CHECK: emitrust.global_load @ops : !emitrust.struct<"Ops">
// CHECK: emitrust.member {{.*}}["bin"]
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}, %{{.*}}) : (!emitrust.fn_ptr<(i32, i32) -> i32>, i32, i32) -> i32
int call_field(void) { return ops.bin(2, 3); }
