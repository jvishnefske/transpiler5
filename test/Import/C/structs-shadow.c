// CTS-R4: block-scope struct declarations shadowing an outer tag. C tag
// identity is (tag name, scope) — C99 6.2.1 gives every block-scope
// declaration of a tag its own type — so the importer keys record identity
// on the defining decl and emits each block-scope definition under the
// function-local-static mangling convention `<function>_<tag>`
// (`_<n>`-suffixed when that name is already taken). The file-scope tag
// keeps its bare name and its cross-TU name/shape dedup.
// RUN: emitrust-import-c %s | FileCheck %s

struct T {
  int x;
};

// A block-scope `struct T` with a different shape shadows the file-scope
// tag: the outer variable keeps the file-scope type, the inner variable
// gets the mangled per-declaration type, and member access resolves
// against the right definition in each scope.
int shadow_diff(void) {
  struct T outer;
  outer.x = 1;
  {
    struct T { int y; } inner;
    inner.y = 2;
    return outer.x + inner.y;
  }
}

// A block-scope `struct T` with the SAME shape as the file-scope tag is
// still a distinct C type per its own declaration, so it still gets its
// own mangled struct_def.
int shadow_same(void) {
  struct T { int x; } local;
  local.x = 3;
  return local.x;
}

// Two shadowing declarations of the same tag in one function: the second
// mangled name is deterministically `_<n>`-suffixed.
int shadow_twice(void) {
  struct T { int a; } first;
  first.a = 4;
  {
    struct T { int b; } second;
    second.b = 5;
    return first.a + second.b;
  }
}

// A tag-only block-scope declaration (no declarator, never used) still
// imports as its own struct_def instead of tripping the cross-TU
// conflict diagnostic.
int shadow_unused(void) {
  struct T v;
  { struct T { int z; }; }
  v.x = 6;
  return v.x;
}

// One struct_def per declaration — the file-scope tag under its bare name,
// each block-scope declaration under its mangled name — and member access
// resolves against the declaration in scope: the outer variable is the
// file-scope type, the inner one the mangled type. (Block-scope
// struct_defs are emitted at the module tail as their functions import,
// so each one is checked inside its function's CHECK-LABEL section.)
// CHECK: emitrust.struct_def @T ["x"] [i32]

// CHECK-LABEL: func.func @shadow_diff
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"T">>
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"shadow_diff_T">>
// CHECK: emitrust.member %{{.*}}["y"] : (!emitrust.lvalue<!emitrust.struct<"shadow_diff_T">>)
// CHECK: emitrust.struct_def @shadow_diff_T ["y"] [i32]

// CHECK-LABEL: func.func @shadow_same
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"shadow_same_T">>
// CHECK: emitrust.struct_def @shadow_same_T ["x"] [i32]

// CHECK-LABEL: func.func @shadow_twice
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"shadow_twice_T">>
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"shadow_twice_T_2">>
// CHECK-DAG: emitrust.struct_def @shadow_twice_T ["a"] [i32]
// CHECK-DAG: emitrust.struct_def @shadow_twice_T_2 ["b"] [i32]

// CHECK-LABEL: func.func @shadow_unused
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"T">>
// CHECK: emitrust.struct_def @shadow_unused_T ["z"] [i32]
