// RUN: emitrust-import-c %s | FileCheck %s

// W2.0: C subset compiled as C++ (per-input language selection picks
// `-x c++ -std=c++17` for a `.cpp` extension, dropping the C path's
// `-std=c11`). Exercises namespace symbol flattening (nested namespaces
// compose the prefix), `extern "C"` linkage preservation (unchanged
// name), `bool`/`true`/`false`, and a plain data-only `struct`/`class`
// (with access specifiers) importing exactly like a C struct. Methods,
// base classes, and references are pinned separately as rejections in
// cpp-basics-invalid.cpp.

namespace shapes {
int square(int side) {
  return side * side;
}

int base_area = 10;

namespace detail {
int helper(int x) {
  return x + 1;
}
} // namespace detail
} // namespace shapes

extern "C" {
int c_linked_add(int a, int b) {
  return a + b;
}
}

struct Point {
public:
  int x;
  int y;
};

class Pair {
public:
  int a;

private:
  int b;
};

bool is_positive(int x) {
  bool result = x > 0;
  return result;
}

bool always_true(void) {
  return true;
}

bool always_false(void) {
  return false;
}

int use_namespaces(void) {
  int total = shapes::square(3);
  total += shapes::base_area;
  total += shapes::detail::helper(4);
  return total;
}

int use_extern_c(void) {
  return c_linked_add(1, 2);
}

int use_point(void) {
  struct Point p;
  p.x = 3;
  p.y = 4;
  return p.x + p.y;
}

int use_pair(void) {
  Pair p;
  p.a = 5;
  return p.a;
}

// Declaration order in the module mirrors source order (types/globals
// import lazily, at first use, so the two struct_defs land after every
// function that never touches them and right before `is_positive`, the
// first one that does). Namespace flattening: `ns_<name>_` per level,
// outer-to-inner, composed for nested namespaces.
// CHECK-LABEL: func.func @ns_shapes_square

// CHECK: emitrust.global @ns_shapes_base_area

// CHECK-LABEL: func.func @ns_shapes_ns_detail_helper

// `extern "C"` preserves the plain C name: no namespace prefix, no
// mangling.
// CHECK-LABEL: func.func @c_linked_add

// A plain `struct` and a `class` with access specifiers both import as an
// ordinary field-only struct_def: access specifiers are ignored (the
// field walk only visits FieldDecl entries) and there are no base
// classes or methods here to reject or skip.
// CHECK: emitrust.struct_def @Point ["x", "y"] [i32, i32]
// CHECK: emitrust.struct_def @Pair ["a", "b"] [i32, i32]

// `bool` maps to i1 regardless of language; C++'s `true`/`false`
// keywords (a distinct CXXBoolLiteralExpr AST node, unlike C's
// stdbool.h macros) fold to the same i1 constants a `_Bool` literal
// would.
// CHECK-LABEL: func.func @is_positive
// CHECK-SAME: -> i1
// CHECK: arith.cmpi sgt
// CHECK-LABEL: func.func @always_true
// CHECK: arith.constant true
// CHECK-LABEL: func.func @always_false
// CHECK: arith.constant false

// A call through a namespace-qualified name resolves to the same
// flattened symbol its definition used.
// CHECK-LABEL: func.func @use_namespaces
// CHECK: call @ns_shapes_square
// CHECK: emitrust.global_load @ns_shapes_base_area : i32
// CHECK: call @ns_shapes_ns_detail_helper

// CHECK-LABEL: func.func @use_extern_c
// CHECK: call @c_linked_add

// A local of struct/class type with no explicit initializer is plain
// uninitialized storage, exactly like C: the field writes below are the
// only assignments, with no synthesized zero-fill from the implicit C++
// default constructor (W2.0's `significantInit` strips that vacuous
// `CXXConstructExpr` before it can be mistaken for a real initializer).
// CHECK-LABEL: func.func @use_point
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Point">>
// CHECK: emitrust.member %{{.*}}["x"]
// CHECK: emitrust.member %{{.*}}["y"]

// CHECK-LABEL: func.func @use_pair
// CHECK: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Pair">>
// CHECK: emitrust.member %{{.*}}["a"]
