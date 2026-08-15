// FR-73 boundary (C++ leg): the underscore fold must not merge `a::_f`
// with `a::f` — both compose to `ns_a_f`, and a silent unification would
// bind one spelling's calls to the other's body. Located rejection at the
// later declaration, same guard and wording as the C per-TU-tag case.
// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

namespace a {
int _f(int x) { return x + 1; }
int f(int x) { return x + 2; }
}

int main() { return a::_f(1) + a::f(2); }

// CHECK: cpp-ns-underscore-invalid.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function name 'f' emits as 'ns_a_f', which collides with '_f' (leading underscores fold into the symbol prefix)
