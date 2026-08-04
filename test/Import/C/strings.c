// RUN: emitrust-import-c %s | FileCheck %s

int printf(const char *fmt, ...);
int puts(const char *s);
int putchar(int c);

// A file-scope `char s[] = "..."` folds to a typed i8 element list through
// the constant-evaluator path, including the implicit terminating NUL.
char greeting[] = "hi";
// CHECK: emitrust.global @greeting <[104 : i8, 105 : i8, 0 : i8]> : !emitrust.array<3xi8>

int main(void) {
  // A block-scope `char s[N] = "..."` assigns the literal's bytes plus the
  // terminating NUL element by element (C99 6.7.8p14); elements past the
  // literal keep the place's default zero value.
  char buf[4] = "ab";
  // CHECK: %[[BUF:.*]] = emitrust.variable named "buf" : !emitrust.lvalue<!emitrust.array<4xi8>>
  // CHECK: emitrust.subscript %[[BUF]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.lvalue<i8>
  // CHECK: arith.constant 97 : i8
  // CHECK: arith.constant 98 : i8
  // CHECK: arith.constant 0 : i8
  // CHECK-NOT: emitrust.subscript %[[BUF]]{{.*}}i8>{{$}}

  // A %s string literal argument becomes an `emitrust.literal` holding a
  // `&'static str`, consumed by an ordinary `{}` placeholder.
  printf("%s says %s\n", "she", "hi");
  // CHECK: %[[L0:.*]] = emitrust.literal "\22she\22" : !emitrust.opaque<"&'static str">
  // CHECK: %[[L1:.*]] = emitrust.literal "\22hi\22" : !emitrust.opaque<"&'static str">
  // CHECK: emitrust.call_opaque "println!"(%[[L0]], %[[L1]]) {args = ["{} says {}", 0 : index, 1 : index]}

  // A %s char-array lvalue is borrowed whole and rendered by the
  // on-demand __emitrust_cstr helper, which stops at the first NUL like C.
  printf("%s\n", buf);
  // CHECK: %[[SL:.*]] = emitrust.slice_of %[[BUF]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
  // CHECK: %[[S:.*]] = emitrust.call_opaque "__emitrust_cstr"(%[[SL]]) : (!emitrust.ref<!emitrust.slice<i8>>) -> !emitrust.opaque<"String">
  // CHECK: emitrust.call_opaque "println!"(%[[S]]) {args = ["{}", 0 : index]}

  // A global char array stages the usual whole-value local copy first.
  printf("%s\n", greeting);
  // CHECK: emitrust.global_load @greeting
  // CHECK: emitrust.call_opaque "__emitrust_cstr"

  // Definition-less puts lowers to println! through the same %s machinery
  // (both literal and char-array shapes); braces and quotes are escaped
  // for the Rust literal spelling.
  puts("brace {x} quote \"q\"");
  // CHECK: %[[P:.*]] = emitrust.literal "\22brace {x} quote \\\22q\\\22\22" : !emitrust.opaque<"&'static str">
  // CHECK: emitrust.call_opaque "println!"(%[[P]]) {args = ["{}", 0 : index]}
  puts(buf);
  // CHECK: emitrust.call_opaque "__emitrust_cstr"
  // CHECK: emitrust.call_opaque "println!"

  // Definition-less putchar lowers to print! of the argument routed
  // through __emitrust_fmt_c (C converts to unsigned char).
  putchar('A');
  // CHECK: %[[CH:.*]] = emitrust.call_opaque "__emitrust_fmt_c"(%{{.*}}) : (i32) -> !emitrust.opaque<"char">
  // CHECK: emitrust.call_opaque "print!"(%[[CH]]) {args = ["{}", 0 : index]}
  return 0;
}

// Both helpers are emitted once at module level; __emitrust_cstr honors
// C's stop-at-first-NUL semantics via take_while.
// CHECK: emitrust.verbatim "fn __emitrust_fmt_c(x: i32) -> char
// CHECK: emitrust.verbatim "fn __emitrust_cstr(s: &[i8]) -> String
// CHECK-SAME: take_while
