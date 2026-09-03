// CTS-P9 (00186): sprintf with a literal format string lowers through the
// same format translation as printf into a Rust `format!` producing a
// String, plus a one-per-module `__emitrust_sprintf` helper (mirroring the
// __emitrust_cstr convention) that copies the bytes and a NUL terminator
// into the destination i8 slice and returns the written length; the
// destination is borrowed mutably from its cursor like the <string.h>
// helpers. `%02d` zero-pad widths translate exactly as in printf.
// RUN: emitrust-import-c %s | FileCheck %s

#include <stdio.h>

int main(void) {
  char buf[32];
  int n = sprintf(buf, "->%02d:%d<-", 7, 42);
  int total = n + 1;
  printf("%s\n", buf);
  printf("%d\n", total);
  return 0;
}

// CHECK-LABEL: func.func @c_main() -> i32
// CHECK: %[[BUF:.*]] = emitrust.variable named "buf" : !emitrust.lvalue<!emitrust.array<32xi8>>

// The format string translates like printf's (%02d -> {:02}, %d -> {})
// into a String-producing format! call; the destination array is borrowed
// as a mutable byte slice at its cursor and both feed the helper, whose
// i32 result is the C return value of sprintf.
// CHECK-DAG: %[[S:.*]] = emitrust.call_opaque "format!"(%{{.*}}, %{{.*}}) {args = ["->{:02}:{}<-", 0 : index, 1 : index]} : (i32, i32) -> !emitrust.opaque<"String">
// CHECK-DAG: %[[DST:.*]] = emitrust.slice_of mut %[[BUF]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<32xi8>>, i64) -> !emitrust.mut_ref<!emitrust.slice<i8>>
// CHECK: %[[N:.*]] = emitrust.call_opaque "__emitrust_sprintf"(%[[DST]], %{{.*}}) : (!emitrust.mut_ref<!emitrust.slice<i8>>, {{.*}}) -> i32

// The returned length participates in ordinary arithmetic.
// CHECK: arith.addi %{{.*}}, %{{.*}} : i32

// The filled buffer reads back through the existing %s machinery -- since
// FR-191 that is the RAW-BYTES funnel (the Latin-1 Display funnel it
// replaced re-encoded every byte >= 0x80 as two UTF-8 bytes). Note the
// asymmetry: `sprintf` itself keeps `format!`, because its result is copied
// into a char buffer rather than written to stdout, so the stdout-only
// `*_out` helpers are unavailable to it.
// CHECK: emitrust.call_opaque "__emitrust_cstr_out"
// CHECK: emitrust.call_opaque "println!"

// The helper is emitted once at module level as safe Rust: it copies the
// formatted bytes plus a NUL terminator into the destination slice
// (overflow panics via the bounds check) and returns the length.
// CHECK: emitrust.verbatim "fn __emitrust_sprintf(dest: &mut [i8], s: &str) -> i32
// CHECK-NOT: unsafe
