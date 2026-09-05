// RUN: emitrust-import-c %s | FileCheck %s

// FR-196: A MEMBER PROJECTED OUT OF A std::vector ELEMENT PLACE IS
// PROJECTED OUT OF THE PLACE ITSELF, NEVER OUT OF A STAGED COPY.
//
// `v[i]` is a `CXXOperatorCallExpr` and `v.at(i)` / `v.front()` /
// `v.back()` are `CXXMemberCallExpr`s -- so all four are
// `clang::CallExpr`s, and `emitMemberBasePlace`'s `f().m` branch used to
// claim every one of them: `emitrust.subscript` -> `emitrust.load` ->
// `emitrust.variable` -> `emitrust.assign` -> `emitrust.member` ON THE
// COPY. For a read that is wasteful; for a WRITE the copy was never read
// back, dead-store elimination deleted both halves, and the assignment
// statement vanished from the emitted crate (measured: native `99`,
// emitted `1`, crate built clean). This is the identical trap FR-189
// found one container over on `(*p).field` over a std::unique_ptr.
//
// So the invariant this file pins is STRUCTURAL, and the `CHECK-NOT`s are
// the load-bearing lines: between the `emitrust.subscript` that opens the
// element place and the `emitrust.member` that refines it there must be
// NO `emitrust.load` and NO `emitrust.variable`. The RUNTIME consequence
// -- that the store actually lands -- is pinned by the byte-diff in
// test/EndToEnd/stl-vector-element-member-write.cpp, because a FileCheck
// cannot see a store that was never emitted, which is exactly how this
// defect survived a green gate.
//
// The last function is the CONTROL: `f().m` on a genuine BY-VALUE
// (prvalue) call result has no place of its own and MUST keep the
// staged-copy path. The `isLValue` guard in `matchStlElementPlaceCall` is
// what separates the two, and this is where its absence would show.

#include <vector>

struct Inner {
  int a;
};

struct P {
  int x;
  Inner in;
  int arr[2];
};

struct Q {
  int x;
};

static Q makeq(int a) {
  Q q;
  q.x = a;
  return q;
}

// `v[i].x = n` -- the headline. One subscript, one member, one assign.
// CHECK-LABEL: func.func @sub_write
// CHECK: %[[SUB:.*]] = emitrust.subscript %{{.*}} -> !emitrust.lvalue<!emitrust.struct<"P">>
// CHECK-NOT: emitrust.load %[[SUB]]
// CHECK-NOT: emitrust.variable
// CHECK: %[[FLD:.*]] = emitrust.member %[[SUB]]["x"]
// CHECK: emitrust.assign %[[FLD]] =
void sub_write(std::vector<P> &v, int n) { v[n].x = n; }

// `v.at(i).x = n` resolves to the IDENTICAL place as the bracket
// spelling: the bounds check is Rust's own on `Vec` indexing.
// CHECK-LABEL: func.func @at_write
// CHECK: %[[ASUB:.*]] = emitrust.subscript %{{.*}} -> !emitrust.lvalue<!emitrust.struct<"P">>
// CHECK-NOT: emitrust.load %[[ASUB]]
// CHECK-NOT: emitrust.variable
// CHECK: %[[AFLD:.*]] = emitrust.member %[[ASUB]]["x"]
// CHECK: emitrust.assign %[[AFLD]] =
void at_write(std::vector<P> &v, int n) { v.at(n).x = n; }

// `v.front().x = n` -- the constant-0 element place (W2.6).
// CHECK-LABEL: func.func @front_write
// CHECK: %[[C0:.*]] = arith.constant 0 : index
// CHECK: %[[FSUB:.*]] = emitrust.subscript %{{.*}}[%[[C0]]] : {{.*}} -> !emitrust.lvalue<!emitrust.struct<"P">>
// CHECK-NOT: emitrust.load %[[FSUB]]
// CHECK-NOT: emitrust.variable
// CHECK: %[[FFLD:.*]] = emitrust.member %[[FSUB]]["x"]
// CHECK: emitrust.assign %[[FFLD]] =
void front_write(std::vector<P> &v, int n) { v.front().x = n; }

// `v.back().x = n` -- the `len() - 1` element place (W2.6).
// CHECK-LABEL: func.func @back_write
// CHECK: emitrust.method_call %{{.*}}["len"]
// CHECK: %[[BSUB:.*]] = emitrust.subscript %{{.*}} -> !emitrust.lvalue<!emitrust.struct<"P">>
// CHECK-NOT: emitrust.load %[[BSUB]]
// CHECK-NOT: emitrust.variable
// CHECK: %[[BFLD:.*]] = emitrust.member %[[BSUB]]["x"]
// CHECK: emitrust.assign %[[BFLD]] =
void back_write(std::vector<P> &v, int n) { v.back().x = n; }

// A NESTED member and an ARRAY-MEMBER subscript off the element place:
// both refine the same subscript place further, still with no copy.
// CHECK-LABEL: func.func @nested_write
// CHECK: %[[NSUB:.*]] = emitrust.subscript %{{.*}} -> !emitrust.lvalue<!emitrust.struct<"P">>
// CHECK-NOT: emitrust.load %[[NSUB]]
// CHECK-NOT: emitrust.variable
// CHECK: %[[NIN:.*]] = emitrust.member %[[NSUB]]["in_"]
// CHECK: %[[NA:.*]] = emitrust.member %[[NIN]]["a"]
// CHECK: emitrust.assign %[[NA]] =
// CHECK: %[[ASUB2:.*]] = emitrust.subscript %{{.*}} -> !emitrust.lvalue<!emitrust.struct<"P">>
// CHECK: %[[ARR:.*]] = emitrust.member %[[ASUB2]]["arr"]
// CHECK: %[[ELT:.*]] = emitrust.subscript %[[ARR]]
// CHECK: emitrust.assign %[[ELT]] =
void nested_write(std::vector<P> &v, int n) {
  v[n].in.a = n;
  v[n].arr[1] = n;
}

// `g[i][j].x = n`: the OUTER element place is itself an element place, so
// the new base resolution has to recurse. Two nested subscripts, then the
// member -- and still no staged copy of either the row or the element.
// CHECK-LABEL: func.func @doubly_indexed_write
// CHECK: %[[ROW:.*]] = emitrust.subscript %{{.*}} -> !emitrust.lvalue<!emitrust.opaque<"Vec<P>">>
// CHECK: %[[CELL:.*]] = emitrust.subscript %[[ROW]]{{.*}} -> !emitrust.lvalue<!emitrust.struct<"P">>
// CHECK-NOT: emitrust.load %[[CELL]]
// CHECK-NOT: emitrust.variable
// CHECK: %[[CFLD:.*]] = emitrust.member %[[CELL]]["x"]
// CHECK: emitrust.assign %[[CFLD]] =
void doubly_indexed_write(std::vector<std::vector<P>> &g, int n) {
  g[n][n].x = n;
}

// A READ takes the same place. Before FR-196 this loaded the WHOLE
// element into a temporary and read the field off that -- rustc E0507 as
// soon as the element stops being `Copy`.
// CHECK-LABEL: func.func @elem_read
// CHECK: %[[RSUB:.*]] = emitrust.subscript %{{.*}} -> !emitrust.lvalue<!emitrust.struct<"P">>
// CHECK-NOT: emitrust.variable
// CHECK: %[[RFLD:.*]] = emitrust.member %[[RSUB]]["x"]
// CHECK: emitrust.load %[[RFLD]]
int elem_read(std::vector<P> &v, int n) { return v[n].x; }

// CONTROL: a by-value call result is a prvalue with no place of its own,
// so `f().m` KEEPS the materialize-into-a-temporary path (CTS 00204).
// CHECK-LABEL: func.func @call_read
// CHECK: %[[CALL:.*]] = call @makeq
// CHECK: %[[TMP:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"Q">>
// CHECK: emitrust.assign %[[TMP]] = %[[CALL]]
// CHECK: emitrust.member %[[TMP]]["x"]
int call_read(int n) { return makeq(n).x; }
