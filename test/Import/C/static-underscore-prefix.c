// FR-73: the per-TU statics tag must compose onto a leading-underscore C
// name WITHOUT manufacturing consecutive underscores. rustc's denied
// non_snake_case lint rejects `tu0__helper` outright in the emitted
// crate, so the boundary folds: `tu0_` + `_helper` -> `tu0_helper`, and
// ALL leading underscores collapse into the one boundary underscore
// (`__twice` -> `tu0_twice`); the same fold applies to a static global
// (`_g` -> `tu0_g`). Non-static names keep their leading underscore
// verbatim (`_pub` is legal snake_case — only prefix concatenation ever
// manufactured the illegal `__`), and per-TU distinctness survives the
// fold (this TU's `_helper` is tu0_helper, the companion's is tu1_helper).
// RUN: emitrust-import-c %s %S/Inputs/static-underscore-other.c | FileCheck %s

extern int use_other(int x);

static int _helper(int x) { return x + 1; }
static int __twice(int x) { return x * 2; }
static int _g = 5;

int _pub(int x) { return x + 3; }

int main(void) {
  return _pub(_helper(__twice(_g))) + use_other(2);
}

// The folded statics: one boundary underscore, however many the C name led
// with.
// CHECK-DAG: func.func @tu0_helper
// CHECK-DAG: func.func @tu0_twice
// CHECK-DAG: emitrust.global @tu0_g <5 : i32>
// External names are untouched: a leading underscore alone is legal.
// CHECK-DAG: func.func @_pub
// CHECK-DAG: func.func @c_main
// The companion TU's identically named file-static stays distinct.
// CHECK-DAG: func.func @tu1_helper
