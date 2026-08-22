// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/keyword-name.cpp 2>&1 | FileCheck %s --check-prefix=KWNAME
// RUN: not emitrust-import-c %t/big-value.cpp 2>&1 | FileCheck %s --check-prefix=BIGVAL
// RUN: not emitrust-import-c %t/empty.cpp 2>&1 | FileCheck %s --check-prefix=EMPTY
// RUN: not emitrust-import-c %t/nonliftable-default.cpp 2>&1 | FileCheck %s --check-prefix=NLDEFAULT

// FR-113 RETIRED the blanket scoped-enumeration rejection this file's first
// two legs used to pin: `enum class`/`enum struct` now import as their
// underlying-typed C-like image (test/Import/Cpp/cpp-enum-class.cpp pins
// the admission). What this file pins instead is that the RESIDUAL enum
// definition gates -- keyword-named enums, values outside i32, an empty
// enum (expressible only in C++) -- still reject scoped definitions with
// the same LOCATED wordings the unscoped gates carry, at the definition,
// not as a downstream type mismatch. The NLDEFAULT leg is unrelated to
// enums and survives from the original Stage-D file: a defaulted argument
// the importer cannot lift rejects with a real file:line:col location
// (recursed into the default value expression).

//--- keyword-name.cpp
// KWNAME: keyword-name.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enum name 'match' is a Rust keyword
enum class match { First, Second };
int use(match m) {
  return static_cast<int>(m);
}

//--- big-value.cpp
// BIGVAL: big-value.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enumerator value does not fit in i32
enum class Big : long long { V = 3000000000LL };
int use(Big b) {
  return 0;
}

//--- empty.cpp
// EMPTY: empty.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enum with no enumerators
enum class Empty {};
int use(Empty e) {
  return 0;
}

//--- nonliftable-default.cpp
struct Widget {
  int x;
};
// The default value is a value-position construction, which does not lift; the
// rejection must now carry the default expression's own location.
// NLDEFAULT: nonliftable-default.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: constructor in value position (only a trivial copy or move is modeled)
int take(int a, Widget w = Widget()) {
  return a + w.x;
}
int use() {
  return take(5);
}
