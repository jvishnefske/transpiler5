// RUN: emitrust-import-c %s | FileCheck %s

// C99-29: the `__func__`-family predefined identifiers (C99 6.4.2.2 —
// plus clang's __FUNCTION__ and __PRETTY_FUNCTION__ synonyms) carry
// their function-name StringLiteral inside the PredefinedExpr, so every
// modeled string-literal position accepts them identically to a spelled
// literal: printf/puts '%s' arguments, char-pointer bindings (the
// C99-28/CTS-P1 read-only literal region), and string-helper arguments.
// Uses outside those positions keep located rejections
// (func-name-invalid.c).

int printf(const char *, ...);
int puts(const char *s);
int strlen(const char *);

// A '%s' argument prints the function name through the literal path: the
// name becomes an `emitrust.literal` &'static str, exactly as if
// "show" had been spelled at the call site.
void show(void) {
  printf("%s\n", __func__);
}
// CHECK-LABEL: func.func @show
// CHECK: %[[NAME:.*]] = emitrust.literal "\22show\22" : !emitrust.opaque<"&'static str">
// CHECK: emitrust.call_opaque "println!"(%[[NAME]]) {args = ["{}", 0 : index]}

// __FUNCTION__ is the same name; puts routes through the same literal
// path onto println!. __PRETTY_FUNCTION__ carries clang's full-signature
// spelling instead of the bare name.
void alias_forms(void) {
  printf("in %s\n", __FUNCTION__);
  puts(__func__);
  printf("%s\n", __PRETTY_FUNCTION__);
}
// CHECK-LABEL: func.func @alias_forms
// CHECK: emitrust.literal "\22alias_forms\22"
// CHECK: emitrust.call_opaque "println!"
// CHECK: emitrust.literal "\22alias_forms\22"
// CHECK: emitrust.call_opaque "println!"
// CHECK: emitrust.literal "\22void alias_forms(void)\22"
// CHECK: emitrust.call_opaque "println!"

// A char pointer bound to __func__ is a cursor into the name's read-only
// literal region: the backing byte array holds the name's bytes plus the
// terminating NUL ('w','a','l','k',0), and the strlen-style walk is
// ordinary cursor arithmetic over it.
int walk(void) {
  const char *p = __func__;
  int n = 0;
  while (*p != 0) {
    n = n + 1;
    p++;
  }
  return n;
}
// CHECK-LABEL: func.func @walk
// CHECK: %[[CELL:.*]] = memref.alloca() : memref<i64>
// CHECK: %[[LIT:.*]] = emitrust.variable const <[119 : i8, 97 : i8, 108 : i8, 107 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<5xi8>>
// CHECK: %[[ZERO:.*]] = arith.constant 0 : i64
// CHECK: memref.store %[[ZERO]], %[[CELL]][] : memref<i64>
// CHECK: emitrust.subscript %[[LIT]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<5xi8>>, i64) -> !emitrust.lvalue<i8>

// A definition-less strlen over __func__ takes the string-helper literal
// path: the name's backing is sliced whole for __emitrust_strlen.
int len(void) {
  return strlen(__func__);
}
// CHECK-LABEL: func.func @len
// CHECK: %[[BACK:.*]] = emitrust.variable const <[108 : i8, 101 : i8, 110 : i8, 0 : i8]> : !emitrust.lvalue<!emitrust.array<4xi8>>
// CHECK: %[[SLICE:.*]] = emitrust.slice_of %[[BACK]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
// CHECK: emitrust.call_opaque "__emitrust_strlen"(%[[SLICE]]) : (!emitrust.ref<!emitrust.slice<i8>>) -> i64
