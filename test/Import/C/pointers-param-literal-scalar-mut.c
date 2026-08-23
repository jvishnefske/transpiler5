// FR-121: a string-literal element handed to a MUTABLE scalar-reference
// parameter. A deref-only `const char *` parameter maps to `&mut i8`
// (mapParamType leaves non-u8 const pointees mutable, FR-55 scope), so the
// call site must not borrow the literal's cached CONST backing mutably:
// `emitrust.addr_of mut` on an element of an `emitrust.variable const`
// renders `&mut v[..]` against a non-`mut` `let` -- rustc E0596, an emitted
// crate that cannot compile (the spdlog assert-fail shape,
// `sink(__FILE__, line, "msg")`, twice per unit in 6 of 9 units).
//
// The call site instead rematerializes a FRESH mutable backing for this
// call -- the exact model the mutable-slice branch already uses for the
// sliced spelling of the same argument (pointers-param-literal-slice.c):
// writing through a pointer to a string literal is undefined behavior, so
// the per-call copy is unobservable to any defined program. The cached
// const backing itself is untouched, so shared readers of the same literal
// keep borrowing the original.
//
// RUN: emitrust-import-c %s | FileCheck %s

int printf(const char *, ...);

static int first(const char *s) { return *s; }

// CHECK-LABEL: func.func @c_main
int main(void) {
  // The literal's cached backing stays const; the call clones a fresh
  // NON-const copy (no `const` marker) and borrows THAT mutably.
  // CHECK: emitrust.variable const <[65 : i8, 122 : i8, 0 : i8]>
  // CHECK: %[[COPY:.+]] = emitrust.variable <[65 : i8, 122 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<3xi8>>
  // CHECK: %[[ELT:.+]] = emitrust.subscript %[[COPY]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<3xi8>>, i64) -> !emitrust.lvalue<i8>
  // CHECK: emitrust.addr_of mut %[[ELT]] : (!emitrust.lvalue<i8>) -> !emitrust.mut_ref<i8>
  printf("%d\n", first("Az"));
  return 0;
}
