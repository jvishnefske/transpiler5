// RUN: emitrust-import-c %s | FileCheck %s --implicit-check-not='"__emitrust_cstr"' --implicit-check-not='"__emitrust_fmt_c"'

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

  // A %s char-array lvalue is borrowed whole and written by the on-demand
  // __emitrust_cstr_out helper, which stops at the first NUL like C.
  // FR-191: the Latin-1 __emitrust_cstr Display funnel this replaced mapped
  // each byte to a Rust `char`, emitting TWO UTF-8 bytes for every byte
  // >= 0x80 where C writes one (measured miscompile). A raw write is not a
  // format hole, so the trailing newline is a bare `println!()`.
  printf("%s\n", buf);
  // CHECK: %[[SL:.*]] = emitrust.slice_of %[[BUF]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xi8>>, i64) -> !emitrust.ref<!emitrust.slice<i8>>
  // CHECK: emitrust.call_opaque "__emitrust_cstr_out"(%[[SL]]) : (!emitrust.ref<!emitrust.slice<i8>>) -> ()
  // CHECK: emitrust.call_opaque "println!"() {args = []}

  // A global char array stages the usual whole-value local copy first.
  printf("%s\n", greeting);
  // CHECK: emitrust.global_load @greeting
  // CHECK: emitrust.call_opaque "__emitrust_cstr_out"

  // Definition-less puts lowers through the same %s machinery (both literal
  // and char-array shapes); braces and quotes are escaped for the Rust
  // literal spelling. FR-191: puts is the same stdout position as
  // `printf("%s\n", ...)`, so a char-array argument writes raw bytes and the
  // bare `println!()` supplies puts' newline.
  puts("brace {x} quote \"q\"");
  // CHECK: %[[P:.*]] = emitrust.literal "\22brace {x} quote \\\22q\\\22\22" : !emitrust.opaque<"&'static str">
  // CHECK: emitrust.call_opaque "println!"(%[[P]]) {args = ["{}", 0 : index]}
  puts(buf);
  // CHECK: emitrust.call_opaque "__emitrust_cstr_out"
  // CHECK: emitrust.call_opaque "println!"() {args = []}

  // FR-194: definition-less putchar writes the argument truncated to ONE
  // BYTE through __emitrust_byte_out. C writes `(unsigned char)c` -- that one
  // character (C11 7.21.7.9) -- for every value 0..255; the __emitrust_fmt_c
  // Display funnel this replaced widened the byte to a Unicode scalar and
  // `Display for char` writes UTF-8, so a byte >= 0x80 came out as two and a
  // getchar/putchar cat loop silently doubled the length of any binary input
  // (measured: native `c8 ff`, emitted `c3 88 c3 bf`). There is no format
  // hole and no second argument, so no print! is emitted at all.
  putchar('A');
  // CHECK: %[[CH:.*]] = arith.trunci %{{.*}} : i32 to i8
  // CHECK: emitrust.call_opaque "__emitrust_byte_out"(%[[CH]]) : (i8) -> ()
  return 0;
}

// Both raw-bytes helpers are emitted once at module level, and both write on
// the same globally buffered stdout handle print! locks; __emitrust_cstr_out
// honors C's stop-at-first-NUL semantics by scanning for the terminator. The
// two Display funnels they replaced (__emitrust_cstr, __emitrust_fmt_c) are
// no longer requested at all -- an unused helper is an `unused` deny in the
// emitted crate -- which the whole-output --implicit-check-not on the RUN
// line pins for both.
// CHECK: emitrust.verbatim "fn __emitrust_cstr_out(s: &[i8]) {
// CHECK-SAME: position(|&b| b == 0)
// CHECK: emitrust.verbatim "fn __emitrust_byte_out(b: i8) {
// CHECK-SAME: write_all(&[b as u8])
