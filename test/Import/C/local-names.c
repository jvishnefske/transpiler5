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

// FR-61e slice 2: a by-value struct parameter's `emitrust.param_names`
// slot is EMPTY -- its shadow variable below already took the C name, and
// two bindings must never share a spelling. With every slot empty the
// attribute is omitted entirely (see shadow_mix below for the slot form).
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

// A plain signed scalar parameter's slot carries its name: mem2reg
// dissolves its cell, so naming the block argument names every use --
// exactly how `n + sum_to(n - 1)` appears.
// CHECK-LABEL: func @sum_to
// CHECK-SAME: emitrust.param_names = ["n"]
int sum_to(int n) { return n <= 0 ? 0 : n + sum_to(n - 1); }

// A keyword-spelled parameter's slot arrives pre-mangled.
// CHECK-LABEL: func @keyword_param
// CHECK-SAME: emitrust.param_names = ["fn_", "b"]
int keyword_param(int fn, int b) { return fn + b; }

// A shadowed (unsigned) parameter's slot is empty while its neighbor
// keeps its name -- the mixed form that pins the empty-slot encoding.
// CHECK-LABEL: func @shadow_mix
// CHECK-SAME: emitrust.param_names = ["", "k"]
int shadow_mix(unsigned u, int k) { return (int)u + k; }
