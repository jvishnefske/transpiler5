// FR-183: import pins for the standard-input reader. The whole feature is
// rendered with NO new op and NO new dialect surface: a `scanf` call is a
// CHAIN of `emitrust.call_opaque` helpers threading a single i32 STATE
// value, one call per format directive, closed by `__emitrust_scan_done`
// which turns the state into C's int return.
//
// The state encoding is the load-bearing part and is why no labelled break
// is needed: `s >= 0` means "still running, s conversions assigned"; `s < 0`
// means "stopped, and the answer is -s - 2". That folds C's entire return
// rule -- the count on a matching failure, -1 (EOF) only when an input
// failure precedes the FIRST conversion -- into arithmetic, so every helper
// can no-op on a stopped state and the chain never branches. It also means
// only ONE `&mut` borrow is live at a time (each helper takes the state plus
// at most one out-reference), so the aliasing analysis is never engaged.
//
// PIN FIRMNESS:
//   FIRM  - the whole file imports (exit 0, every function below present);
//   FIRM  - one helper call PER DIRECTIVE, in format order, each consuming
//           the previous call's result as its first operand (the state
//           thread), and a closing `__emitrust_scan_done`;
//   FIRM  - the out-argument is an `emitrust.addr_of mut` producing a
//           `!emitrust.mut_ref` of the variable's own scalar type -- never a
//           slice, never an FR-40 owner lift;
//   FIRM  - `fscanf(stdin, ...)` and `scanf(...)` produce the SAME chain;
//   FIRM  - `getchar()` is the stdin primitive with no handle operand;
//   LOOSE - helper spellings beyond the names checked here, SSA numbering,
//           and the emitted Rust text.
// RUN: emitrust-import-c %s | FileCheck %s

#include <stdio.h>

// A single %d: state 0 in, one conversion, done.
int one_int(void) {
  int x = 0;
  scanf("%d", &x);
  return x;
}
// CHECK-LABEL: func.func @one_int
// CHECK: %[[R0:.*]] = emitrust.addr_of mut {{.*}} -> !emitrust.mut_ref<i32>
// CHECK-NEXT: %[[S1:.*]] = emitrust.call_opaque "__emitrust_scan_d"(%c0{{[a-z0-9_]*}}, %[[R0]])
// CHECK-NEXT: emitrust.call_opaque "__emitrust_scan_done"(%[[S1]])

// Two conversions with a whitespace directive between them: THREE calls in
// format order, each threading the previous state.
int two_ints(void) {
  int x = 0;
  int y = 0;
  scanf("%d %d", &x, &y);
  return x + y;
}
// CHECK-LABEL: func.func @two_ints
// CHECK: %[[T1:.*]] = emitrust.call_opaque "__emitrust_scan_d"(%c0{{[a-z0-9_]*}}, %{{.*}})
// CHECK-NEXT: %[[T2:.*]] = emitrust.call_opaque "__emitrust_scan_ws"(%[[T1]])
// CHECK-NEXT: %{{.*}} = emitrust.addr_of mut
// CHECK-NEXT: %[[T3:.*]] = emitrust.call_opaque "__emitrust_scan_d"(%[[T2]], %{{.*}})
// CHECK-NEXT: emitrust.call_opaque "__emitrust_scan_done"(%[[T3]])

// %u takes an unsigned out-reference; %c an 8-bit one and NO whitespace
// skip of its own.
int unsigned_and_char(void) {
  unsigned int u = 0;
  char c = 0;
  scanf("%u", &u);
  scanf("%c", &c);
  return (int)u + c;
}
// CHECK-LABEL: func.func @unsigned_and_char
// CHECK: emitrust.addr_of mut {{.*}} -> !emitrust.mut_ref<ui32>
// CHECK-NEXT: emitrust.call_opaque "__emitrust_scan_u"
// CHECK: emitrust.addr_of mut {{.*}} -> !emitrust.mut_ref<i8>
// CHECK-NEXT: emitrust.call_opaque "__emitrust_scan_c"

// The return value IS representable: it is the closing helper's result, so
// a `!= 1` guard is an ordinary comparison.
int checked_read(void) {
  int x = 0;
  if (scanf("%d", &x) != 1)
    return -1;
  return x;
}
// CHECK-LABEL: func.func @checked_read
// CHECK: %[[C:.*]] = emitrust.call_opaque "__emitrust_scan_done"
// CHECK: arith.cmpi ne, %[[C]]

// fscanf on the literal `stdin` is the same reader: same chain, no stream
// operand anywhere (stdin is a process-global, not an owned handle).
int via_fscanf(void) {
  int x = 0;
  fscanf(stdin, "%d", &x);
  return x;
}
// CHECK-LABEL: func.func @via_fscanf
// CHECK: emitrust.call_opaque "__emitrust_scan_d"
// CHECK: emitrust.call_opaque "__emitrust_scan_done"

// getchar() is the stdin byte primitive itself, with no handle operand.
int one_byte(void) { return getchar(); }
// CHECK-LABEL: func.func @one_byte
// CHECK: emitrust.call_opaque "__emitrust_stdin_getc"() : () -> i32

// fgetc/fgets/fread on `stdin` materialize a FRESH stateless handle
// temporary at the use site -- that is what dissolves the ownership
// question a shared global FILE* would otherwise raise.
int stdin_bytes(void) {
  char line[8];
  int c = fgetc(stdin);
  if (fgets(line, 8, stdin) == NULL)
    return -1;
  return c + line[0];
}
// CHECK-LABEL: func.func @stdin_bytes
// CHECK: emitrust.call_opaque "__emitrust_stdin"() : () -> !emitrust.opaque<"__EmitrustFile">
// CHECK: emitrust.call_opaque "__emitrust_fgetc"
// CHECK: emitrust.call_opaque "__emitrust_stdin"() : () -> !emitrust.opaque<"__EmitrustFile">
// CHECK: emitrust.call_opaque "__emitrust_fgets"
