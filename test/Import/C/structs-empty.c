// CTS-R6: an empty struct (`struct T {};`, a GNU/C2x shape clang accepts)
// imports as a field-less struct_def that renders as a unit-like Rust
// struct. Declaring, copying, and passing values of the empty struct works
// through the same variable/load/assign path as any other struct; the
// anonymous-typedef spelling (`typedef struct {} E;`) names it the same way
// as a non-empty typedef struct.
// RUN: emitrust-import-c %s | FileCheck %s

struct Nothing {};

typedef struct {
} Unit;

struct Nothing pass_through(struct Nothing n) { return n; }

int use_empty(void) {
  struct Nothing a;
  struct Nothing b;
  b = a;
  b = pass_through(a);
  Unit u;
  Unit v;
  v = u;
  return 0;
}

// Both spellings import as field-less struct_defs.
// CHECK: emitrust.struct_def @Nothing [] []
// CHECK: emitrust.struct_def @Unit [] []

// A by-value empty-struct parameter copies into a local place and returns.
// CHECK-LABEL: func.func @pass_through
// CHECK-SAME: (%[[N:.*]]: !emitrust.struct<"Nothing">) -> !emitrust.struct<"Nothing">
// CHECK: %[[NV:.*]] = emitrust.variable named "n" : !emitrust.lvalue<!emitrust.struct<"Nothing">>
// CHECK: emitrust.assign %[[NV]] = %[[N]] : !emitrust.lvalue<!emitrust.struct<"Nothing">>
// CHECK: emitrust.load
// CHECK: return

// Declaration, copy assignment, and a call all work on values of the
// empty struct.
// CHECK-LABEL: func.func @use_empty
// CHECK: %[[A:.*]] = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.struct<"Nothing">>
// CHECK: %[[B:.*]] = emitrust.variable named "b" : !emitrust.lvalue<!emitrust.struct<"Nothing">>
// CHECK: emitrust.load %[[A]]
// CHECK: emitrust.assign %[[B]]
// CHECK: call @pass_through
// CHECK: emitrust.variable named "u" : !emitrust.lvalue<!emitrust.struct<"Unit">>
// CHECK: return
