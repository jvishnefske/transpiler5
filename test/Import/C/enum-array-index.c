// RUN: emitrust-import-c %s | FileCheck %s

// FR-149: an ENUM-VALUED ARRAY SUBSCRIPT. C's `tbl[target]` on a
// `LogTarget target` indexes by the enumerator's integer value, but the
// importer keeps a NAMED enum as `!emitrust.enum<"Name">` all the way to
// the use site -- only an ANONYMOUS `typedef enum { ... } T;` decays to a
// plain `i32` in `mapType`, which is why this shape survived so long
// undetected. `emitrust.subscript`'s index operand is constrained to
// integer-or-index, so every named-enum index used to abort the whole
// translation unit with a verifier error:
//   error: 'emitrust.subscript' op operand #1 must be integer or index,
//          but got '!emitrust.enum<"LogTarget">'
// This test pins that a named-enum index is normalized to its i32
// discriminant by an `emitrust.cast` -- the importer's universal
// enum-to-integer conversion, value-preserving because every imported
// enumerator is required to fit in i32 -- at the ONE seam every
// user-written index expression passes through, so no subscript-building
// site can be reached with an enum index. The shapes below deliberately
// span the DIFFERENT SubscriptOp construction sites a per-site fix would
// leak past: a plain array read, a write position, both levels of a
// multi-dimensional array, and a subscript through a POINTER (which folds
// the index into a flat cursor instead of building the op directly, and
// which used to reject outright with "unsupported subscript index type").

typedef enum LogTarget { T_A = 0, T_B = 1, T_C = 2 } LogTarget;
enum Temp { Cold = -1, Warm = 1 };

static const int tbl[3] = {10, 20, 30};

// A read at a named-enum index: the cast to i32 feeds the subscript.
int max_level(LogTarget t) { return tbl[t]; }

// CHECK-LABEL: func.func @max_level
// CHECK: %[[E:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"LogTarget"> to i32
// CHECK: emitrust.subscript %{{[0-9]+}}[%[[E]]] : (!emitrust.lvalue<!emitrust.array<3xi32>>, i32) -> !emitrust.lvalue<i32>

// A WRITE position: `emitLValue` reaches the same seam, so the store
// lands through an identically normalized index.
int store_at(LogTarget t) {
  int a[3] = {0, 0, 0};
  a[t] = 7;
  return a[1];
}

// CHECK-LABEL: func.func @store_at
// CHECK: %[[W:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"LogTarget"> to i32
// CHECK: %[[WP:.*]] = emitrust.subscript %{{[0-9]+}}[%[[W]]] : (!emitrust.lvalue<!emitrust.array<3xi32>>, i32) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[WP]]

// Both levels of a multi-dimensional array: the outer subscript yields an
// array-typed place that the inner one refines, so the seam is crossed
// twice in one expression.
static const int grid[3][2] = {{1, 2}, {3, 4}, {5, 6}};

int two_d(LogTarget t) { return grid[t][1] + grid[1][t]; }

// CHECK-LABEL: func.func @two_d
// CHECK: %[[R:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"LogTarget"> to i32
// CHECK: emitrust.subscript %{{[0-9]+}}[%[[R]]] : (!emitrust.lvalue<!emitrust.array<3x!emitrust.array<2xi32>>>, i32) -> !emitrust.lvalue<!emitrust.array<2xi32>>
// CHECK: %[[C:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"LogTarget"> to i32
// CHECK: emitrust.subscript %{{[0-9]+}}[%[[C]]] : (!emitrust.lvalue<!emitrust.array<2xi32>>, i32) -> !emitrust.lvalue<i32>

// A subscript through a decomposed POINTER: a different construction site
// entirely -- the index is scaled and folded into the pointer's flat
// cursor, never reaching the op's operand -- and it used to be a located
// rejection ("unsupported subscript index type") rather than a verifier
// abort. The pin moves forward: it now emits.
int through_pointer(LogTarget t) {
  const int *p = tbl;
  return p[t];
}

// CHECK-LABEL: func.func @through_pointer
// CHECK: %[[P:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"LogTarget"> to i32
// CHECK: arith.extsi %[[P]] : i32 to i64

// An enum with a NEGATIVE enumerator gets a SIGNED underlying type, so its
// raw field is already i32 and the normalization is the same cast (the
// emitter drops the rendered ` as i32` tail as an identity, but the op is
// what this file pins).
int signed_underlying(enum Temp t) { return tbl[t]; }

// CHECK-LABEL: func.func @signed_underlying
// CHECK: %[[S:.*]] = emitrust.cast %{{[0-9]+}} : !emitrust.enum<"Temp"> to i32
// CHECK: emitrust.subscript %{{[0-9]+}}[%[[S]]] : (!emitrust.lvalue<!emitrust.array<3xi32>>, i32) -> !emitrust.lvalue<i32>

int main(void) {
  return max_level(T_B) + store_at(T_A) + two_d(T_C) + through_pointer(T_B) +
         signed_underlying(Warm);
}
