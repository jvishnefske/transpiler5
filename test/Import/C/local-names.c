// FR-61e slice 1: a decl-bound local in the named subset (aggregates,
// unsigned scalars, keyword-spelled locals among them, and by-value param
// shadows) carries its final Rust base spelling on the `emitrust.variable`
// place via the `named` attribute; the spelling arrives PRE-mangled
// (`mangleMemberName`: snake_case under the idiomatic rename, `_` appended
// to Rust keywords), so the emitter never re-derives it. Plain signed
// non-address-taken scalars stay anonymous (they dissolve under mem2reg
// and re-materialize with no decl association).
// RUN: emitrust-import-c %s | FileCheck %s

struct Pair {
  int a;
  int b;
};

// CHECK-LABEL: func @locals
// The by-value struct parameter's shadow variable is named after it.
// CHECK: emitrust.variable named "p" : !emitrust.lvalue<!emitrust.struct<"Pair">>
int locals(struct Pair p) {
  // An aggregate local carries its C name.
  // CHECK: emitrust.variable named "q" : !emitrust.lvalue<!emitrust.struct<"Pair">>
  struct Pair q = {1, 2};
  // An unsigned scalar local carries its C name.
  // CHECK: emitrust.variable named "count" : !emitrust.lvalue<ui32>
  unsigned int count = 3;
  // A Rust-keyword C name arrives pre-mangled with the trailing `_`.
  // CHECK: emitrust.variable named "match_" : !emitrust.lvalue<ui32>
  unsigned int match = 5;
  return q.a + p.b + (int)count + (int)match;
}
