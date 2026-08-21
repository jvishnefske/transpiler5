// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/variadic.c 2>&1 | FileCheck %s --check-prefix=VARIADIC
// RUN: not emitrust-import-c %t/mismatch.c 2>&1 | FileCheck %s --check-prefix=MISMATCH
// RUN: not emitrust-import-c %t/cast-mismatch.c 2>&1 | FileCheck %s --check-prefix=CASTMISMATCH
// RUN: not emitrust-import-c %t/noproto-args.c 2>&1 | FileCheck %s --check-prefix=NOPROTO
// RUN: emitrust-import-c %t/pointer-component.c | FileCheck %s --check-prefix=COMPONENT

// FR-29: every unsupported function pointer construct fails the import
// with a located diagnostic: a pointer to a variadic function, a function
// bound to a pointer with a different signature (including a
// prototype-less `int (*)()` pointer bound to a function with
// parameters), and a call with arguments through a prototype-less
// pointer.
//
// The last arm is a MOVED FRONTIER. It pinned a fn_ptr whose component
// types fall outside the supported set, most recently a STRUCT-pointer
// component; FR-102 admits that shape, so the arm advanced to the
// POSITIVE spelling and now pins what matters about it here — that the
// BINDING still goes through the FR-29 signature-equality check at
// `resolveFunctionPointerDecl`, i.e. `taker`'s own imported signature
// must equal the component type exactly for `Some(taker)` to be emitted.
// The rejecting flavors of the component frontier live in
// fnptr-struct-components-invalid.c.

//--- variadic.c
int printf(const char *, ...);
int main(void) {
  int (*fp)(const char *, ...) = printf;
  return 0;
}
// VARIADIC: error: unsupported: variadic function pointer type

//--- mismatch.c
int add(int a, int b) { return a + b; }
int main(void) {
  int (*np)() = add;
  return np();
}
// MISMATCH: error: unsupported: function 'add' does not match the function pointer signature

//--- cast-mismatch.c
int add(int a, int b) { return a + b; }
int main(void) {
  int (*fp)(int) = (int (*)(int))add;
  return fp(1);
}
// CASTMISMATCH: error: unsupported: function 'add' does not match the function pointer signature

//--- noproto-args.c
// Callsite-prototype inference (00209 wave) derives (int, int) -> int
// from the call, so the failure is now the incompatible BINDING of
// `zero` at the initializer, not the call itself. The blanket
// no-prototype-call rejection survives for non-decl-traceable callees
// (pinned in fnptr-noproto-infer-invalid.c's member case).
int zero(void) { return 0; }
int main(void) {
  int (*np)() = zero;
  return np(1, 2);
}
// NOPROTO: error: unsupported: function 'zero' does not match the function pointer signature

//--- pointer-component.c
// FR-76 moved arithmetic-pointee scalar-pointer components INTO the
// supported set (`void (*)(int *)` is a fn_ptr over `&mut [i32]`, pinned
// in fnptr-slice-components.c) and FR-102 moved COMPLETE-record pointees
// in after it. `taker` is address-taken with a non-arithmetic pointee, so
// the FR-76 slice forcing leaves it ScalarRef and its signature equals the
// component type — the equality check unifies and the binding emits.
struct S { int x; };
void taker(struct S *p) { p->x = 1; }
int main(void) {
  void (*fp)(struct S *) = taker;
  return 0;
}
// COMPONENT: func.func @taker(%{{.*}}: !emitrust.mut_ref<!emitrust.struct<"S">>)
// COMPONENT: emitrust.constant <#emitrust.opaque<"Some(taker)">> : !emitrust.fn_ptr<(!emitrust.mut_ref<!emitrust.struct<"S">>)>
