// RUN: emitrust-import-c %s | FileCheck %s

// FR-29 / CTS 00209: K&R callsite-prototype inference. A call with
// arguments through a value of pointer-to-FunctionNoProtoType whose
// callee (after deref-peel) traces to a single ParmVarDecl/VarDecl
// INFERS that decl's prototype from the call's default-promoted
// argument types (clang already applies the promotions) plus the
// declared return type. The decl's fn_ptr VALUE type is refined to the
// inferred signature at declaration/parameter mapping, so the call
// lowers through the ordinary typed emitrust.call_indirect path.
// Multiple call sites for one decl must agree. No-proto pointers that
// are never called with arguments keep the unrefined fn_ptr<() -> T>
// mapping (zero-arg calls stay as in fn-pointers.c). Rejections live in
// fnptr-noproto-infer-invalid.c.

int add_three(int x) { return x + 3; }

// The 00209 f1 shape: a no-proto fn_ptr parameter called `(*fp)(i)`.
// The parameter type is refined to the inferred (i32) -> i32 signature
// and the call is the ordinary typed indirect call.
typedef int (*fptr1)();
int f1(fptr1 fp, int i) { return (*fp)(i); }
// CHECK-LABEL: func.func @f1
// CHECK-SAME: (%{{.*}}: !emitrust.fn_ptr<(i32) -> i32>, %{{.*}}: i32) -> i32
// CHECK: emitrust.variable named "fp" : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}) : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32

// A LOCAL no-proto pointer initialized with a real matching function
// and called with an argument: the inference refines the variable type
// and resolveFunctionPointerDecl accepts the binding at the refined
// signature (the opaque Some(target) constant carries the refined
// type).
int local_infer(void) {
  int (*np)() = add_three;
  return np(4);
}
// CHECK-LABEL: func.func @local_infer
// CHECK: emitrust.variable named "np" : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
// CHECK: emitrust.constant <#emitrust.opaque<"Some(add_three)">> : !emitrust.fn_ptr<(i32) -> i32>
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}) : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32

// Two AGREEING call sites for one decl — the deref spelling and the
// plain spelling — infer the same signature and both lower to the same
// typed indirect call.
int two_sites(int (*fp)(), int a, int b) {
  int x = (*fp)(a);
  int y = fp(b);
  return x + y;
}
// CHECK-LABEL: func.func @two_sites
// CHECK-SAME: (%{{.*}}: !emitrust.fn_ptr<(i32) -> i32>, %{{.*}}: i32, %{{.*}}: i32) -> i32
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}) : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}) : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32

// Default argument promotions feed the inference: a char argument
// promotes to int, so the inferred slot is i32 (the promotion cast is
// already in the AST).
int promote_char(int (*fp)(), char c) { return (*fp)(c); }
// CHECK-LABEL: func.func @promote_char
// CHECK-SAME: (%{{.*}}: !emitrust.fn_ptr<(i32) -> i32>, %{{.*}}: i8) -> i32
// CHECK: arith.extsi %{{.*}} : i8 to i32
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}) : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32

// ... and a float argument promotes to double, so the inferred slot is
// f64.
int promote_float(int (*fp)(), float f) { return (*fp)(f); }
// CHECK-LABEL: func.func @promote_float
// CHECK-SAME: (%{{.*}}: !emitrust.fn_ptr<(f64) -> i32>, %{{.*}}: f32) -> i32
// CHECK: arith.extf %{{.*}} : f32 to f64
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}) : (!emitrust.fn_ptr<(f64) -> i32>, f64) -> i32

// The 00209 f5 shape: a no-proto fn_ptr VALUE that is only ever passed
// as an argument (never itself called with arguments) stays at the
// unrefined fn_ptr<() -> i32> type — inference is per-decl, not
// per-typedef, so f5's `i` is untouched by f1's refinement of the same
// fptr1 spelling.
typedef int (*fptr5)(fptr1);
int f5(fptr5 fp, fptr1 i) { return fp(i); }
// CHECK-LABEL: func.func @f5
// CHECK-SAME: (%{{.*}}: !emitrust.fn_ptr<(!emitrust.fn_ptr<() -> i32>) -> i32>, %{{.*}}: !emitrust.fn_ptr<() -> i32>) -> i32
// CHECK: emitrust.call_indirect %{{.*}}(%{{.*}}) : (!emitrust.fn_ptr<(!emitrust.fn_ptr<() -> i32>) -> i32>, !emitrust.fn_ptr<() -> i32>) -> i32
