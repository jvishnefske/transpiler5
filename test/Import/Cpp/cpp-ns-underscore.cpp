// FR-73 (C++ leg): the namespace flattening prefix composes onto a
// leading-underscore name through the same fold as the per-TU statics tag:
// `ns_a_` + `_f` -> `ns_a_f`, never the `ns_a__f` that rustc's denied
// non_snake_case lint rejects. A top-level `_g` keeps its verbatim leading
// underscore — only prefix concatenation ever manufactured the illegal
// `__`.
// RUN: emitrust-import-c %s | FileCheck %s

namespace a {
int _f(int x) { return x + 1; }
}

int _g(int x) { return x + 2; }

int main() { return a::_f(1) + _g(2); }

// CHECK-DAG: func.func @ns_a_f
// CHECK-DAG: func.func @_g
// CHECK-DAG: func.func @c_main
