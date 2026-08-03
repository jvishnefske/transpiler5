// RUN: emitrust-import-c %s | FileCheck %s

// W2.3: STL recognition for std::vector<T> USAGE. `mapType` diverts on a
// RecordType decl living in namespace `std` (`Decl::isInStdNamespace()`)
// BEFORE the generic RecordType path would call `importRecord` and recurse
// into libstdc++ internals — a `ClassTemplateSpecializationDecl` named
// "vector" maps to `!emitrust.opaque<"Vec<<T>>">`, where `<T>` is the
// element type mapped recursively and re-spelled (`rustSpellingForElementType`
// / `parseStlElementType`, an exact round-trip pair). Pins:
//   * default construction -> `Vec::new()` (via `emitrust.call_opaque`,
//     staged through an `emitrust.variable` place and an explicit
//     `emitrust.assign`, exactly like the FILE*/String precedent).
//   * push_back -> `.push(x)` via `emitrust.method_call` (MethodCallOp's
//     receiver-type check is widened this wave to also accept an
//     `!emitrust.opaque` lvalue, not just `!emitrust.struct`).
//   * size() -> `.len()` (which returns `index`/`usize`, NOT the C
//     `size_t` declared by the call expression) then an EXPLICIT
//     `emitrust.cast` to the call expression's OWN mapped type (`size_t`
//     -> `ui64` here); the surrounding `int n = ...` conversion is the
//     ordinary, already-existing ui64->i32 assignment narrowing.
//   * operator[] -> bracket indexing via `emitrust.subscript` (widened
//     this wave to also accept an opaque array operand, trusting the
//     result element type rather than deriving it structurally — there is
//     nothing to derive from an opaque string). The index argument is
//     already ui64 by the time it reaches here: `operator[](size_type)`'s
//     real signature has already converted the `int` literal via an
//     ordinary implicit integral cast, so `emitrust.subscript`'s own `as
//     usize` (added because the index is not `index`-typed) works on an
//     ALREADY-converted operand.
//   * empty() -> `.is_empty()`, clear() -> `.clear()`.
// `at()` pins the IDENTICAL bracket-indexing shape as `operator[]` (a
// deliberate renegotiation — both panic on out-of-bounds in Rust, so C++'s
// throw-vs-UB distinction between them collapses to one Rust panic
// policy). back(), pop_back(), and every other vector method are OUT this
// wave (see stl-invalid.cpp); so is a sized/fill constructor
// (`std::vector<int> v(5);`) and ranged-for (see stl-invalid.cpp).

#include <vector>

int use_vector(void) {
  std::vector<int> v;
  v.push_back(1);
  v.push_back(2);
  int n = v.size();
  int first = v[0];
  int second = v.at(1);
  int e = v.empty();
  v.clear();
  return n + first + second + e;
}

// CHECK-LABEL: func.func @use_vector
// The default-constructed place: an `emitrust.variable` of
// `!emitrust.opaque<"Vec<i32>">`, immediately assigned `Vec::new()`'s
// result (never left to the VariableOp's own dead-code default value).
// CHECK: %[[V:.*]] = emitrust.variable named "v" : !emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>
// CHECK: %[[NEW:.*]] = emitrust.call_opaque "Vec::new"() : () -> !emitrust.opaque<"Vec<i32>">
// CHECK: emitrust.assign %[[V]] = %[[NEW]] : !emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>

// push_back(1) / push_back(2): `.push(x)`, void result.
// CHECK: %[[ONE:.*]] = arith.constant 1 : i32
// CHECK: emitrust.method_call %[[V]]["push"] (%[[ONE]]) : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>, i32) -> ()
// CHECK: %[[TWO:.*]] = arith.constant 2 : i32
// CHECK: emitrust.method_call %[[V]]["push"] (%[[TWO]]) : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>, i32) -> ()

// size(): `.len()` returns `index` (usize); cast to the call's OWN mapped
// type (size_t -> ui64) — a SEPARATE, later cast narrows to `int` for the
// `int n = ...` binding (ordinary, pre-existing conversion machinery).
// CHECK: %[[LEN:.*]] = emitrust.method_call %[[V]]["len"] () : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>) -> index
// CHECK: emitrust.cast %[[LEN]] : index to ui64

// operator[](0): bracket indexing via `emitrust.subscript` + `emitrust.load`.
// CHECK: %[[SUB:.*]] = emitrust.subscript %[[V]][%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>, ui64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load %[[SUB]] : (!emitrust.lvalue<i32>) -> i32

// at(1): the IDENTICAL bracket-indexing shape as operator[] above.
// CHECK: %[[SUB2:.*]] = emitrust.subscript %[[V]][%{{.*}}] : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>, ui64) -> !emitrust.lvalue<i32>
// CHECK: emitrust.load %[[SUB2]] : (!emitrust.lvalue<i32>) -> i32

// empty(): `.is_empty()` -> i1.
// CHECK: emitrust.method_call %[[V]]["is_empty"] () : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>) -> i1

// clear(): `.clear()`, void result.
// CHECK: emitrust.method_call %[[V]]["clear"] () : (!emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>) -> ()
