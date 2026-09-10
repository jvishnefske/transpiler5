// FR-230's frontier C+E: a `strchr`/`strrchr` result BOUND to a pointer, and
// a pointer assignment used as an rvalue. Before this, the only admitted
// consumptions of a search result were a printf `%s` argument and a null
// comparison, so the ordinary C idiom
//
//     for (const char *s = in; s = strchr(s, c); s++) ...
//
// was refused twice over: once by the pointer-region analysis (the call was
// not an address expression) and once by the pointer-expression evaluator
// (an assignment has no pointer decomposition).
//
// WHY THIS IS NOT A NEW MODEL. `__emitrust_strchr(s: &[i8], c: i32) -> i64`
// already answered index-or-`-1` into argument 0's own region, which is
// EXACTLY FR-104's param-cursor return with the plan supplied by the library
// instead of proven from a body. So the search reuses that arm's shape: the
// same region, cursor advanced by the index, and the CTS-P8 non-null flag is
// `index != -1`. Nothing new reaches the dialect.
//
// PIN FIRMNESS:
//   FIRM  - the search lowers to ONE `__emitrust_strchr` call whose i64
//           result is compared against -1 to produce the non-null flag, and
//           whose payload is CLAMPED by that flag before it offsets the
//           argument's cursor (a raw -1 would index backwards);
//   FIRM  - the assignment-as-rvalue STORES BEFORE IT READS: the flag cell
//           and the cursor cell are written, then reloaded, so the condition
//           tests the new binding and not the old one;
//   FIRM  - `strrchr` is the same shape through `__emitrust_strrchr`;
//   FIRM  - a dereference of the possibly-null result carries the CTS-P8
//           guard -- C says the not-found deref is undefined, and a panic is
//           the loud direction;
//   LOOSE - SSA numbering, block structure and the emitted Rust text.
// RUN: emitrust-import-c %s | FileCheck %s

#include <string.h>

// The corpus 028 loop, verbatim: the assignment IS the condition.
int count_for(const char *in, char c) {
  int res = 0;
  for (const char *s = in; s = strchr(s, c); s++)
    res++;
  return res;
}
// CHECK-LABEL: func.func @count_for
// CHECK: %[[IDX:.*]] = emitrust.call_opaque "__emitrust_strchr"
// CHECK: %[[M1:.*]] = arith.constant -1 : i64
// CHECK: %[[FOUND:.*]] = arith.cmpi ne, %[[IDX]], %[[M1]]
// CHECK: %[[PAY:.*]] = arith.select %[[FOUND]], %[[IDX]], %{{.*}} : i64
// CHECK: %[[CUR:.*]] = arith.addi %{{.*}}, %[[PAY]]
// The store-then-read-back that makes the assignment an rvalue.
// CHECK: memref.store %[[FOUND]], %[[FLAG:.*]][] : memref<i1>
// CHECK: memref.store %[[CUR]], %[[CELL:.*]][] : memref<i64>
// CHECK: memref.load %[[CELL]][] : memref<i64>
// CHECK: %[[TEST:.*]] = memref.load %[[FLAG]][] : memref<i1>
// CHECK: cf.cond_br %[[TEST]]

// A plain single bind with an explicit null test: no assignment-as-rvalue
// at all, just the region source.
int bind_once(const char *in, char c) {
  const char *s = strchr(in, c);
  if (s == 0)
    return -1;
  return (int)*s;
}
// CHECK-LABEL: func.func @bind_once
// CHECK: emitrust.call_opaque "__emitrust_strchr"
// CHECK: arith.cmpi ne, %{{.*}}, %{{.*}} : i64
// CHECK: arith.select

// The walking re-assignment as a STATEMENT, which needs only the region
// source; the null test is separate.
int count_stmt(const char *in, char c) {
  const char *s = in;
  int res = 0;
  for (;;) {
    s = strchr(s, c);
    if (s == NULL)
      break;
    res++;
    s++;
  }
  return res;
}
// CHECK-LABEL: func.func @count_stmt
// CHECK: emitrust.call_opaque "__emitrust_strchr"

// `strrchr` is the same decomposition over the reverse helper.
int last_byte(const char *in, char c) {
  const char *s = in;
  if ((s = strrchr(s, c)))
    return (int)*s;
  return -1;
}
// CHECK-LABEL: func.func @last_byte
// CHECK: emitrust.call_opaque "__emitrust_strrchr"
// CHECK: arith.cmpi ne
// CHECK: arith.select

// A dereference with NO null test in the C: the search may return NULL, the
// C deref is then undefined, and the CTS-P8 guard makes it loud instead of
// reading the region's first byte.
int unguarded(const char *in, char c) {
  const char *s = strchr(in, c);
  return (int)*s;
}
// CHECK-LABEL: func.func @unguarded
// CHECK: emitrust.call_opaque "assert!"({{.*}}) {args = [0 : index, "null pointer dereference"]}
