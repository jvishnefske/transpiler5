// RUN: emitrust-import-c %s | FileCheck %s

// FR-107: a pointer-ARRAY struct member (`T *m[N]`) stores as `[i64; N]`
// — one inert cursor slot per element. This is the CTS-P2 scalar rule
// (a data-pointer member is a plain i64 cursor whose value carries no
// runtime information) taken exactly one dimension out, and it admits
// the TYPE only. Nothing binds an element to a target object, so every
// element read, write, address-of, comparison, cast, argument and return
// stays a LOCATED rejection at the use — see
// pointers-member-array-invalid.c, which pins each one.
//
// What the admission buys is CASCADE DISSOLUTION, not ported element
// traffic: a record carrying such a member stops gating every type that
// merely NAMES it. lwIP's `struct netif` is blocked by exactly two of
// them (`void *client_data[N]` in netif.h and `struct stats_mem
// *memp[N]` in stats.h) and by nothing else; admitting the type turns a
// 71-item cascade into ordinary per-item blockers. This file pins the
// shapes the record itself must survive: declaration, every instance
// form, and every initializer form that reaches the slots.

struct T { int tag; };

struct holder {
  int n;
  void *m[4];
  struct T *p[8];
  const char *s[2];
  int k;
};
// CHECK: emitrust.struct_def @T ["tag"] [i32]
// CHECK: emitrust.struct_def @holder ["n", "m", "p", "s", "k"] [i32, !emitrust.array<4xi64>, !emitrust.array<8xi64>, !emitrust.array<2xi64>, i32]

// A record with a pointer-array member is still an ordinary by-pointer
// parameter: the `[i64; N]` slots do not change the receiver shape.
int mtu(struct holder *h) { return h->n + h->k; }
// CHECK-LABEL: func.func @mtu(%arg0: !emitrust.mut_ref<!emitrust.struct<"holder">>)
// CHECK: emitrust.member %{{.*}}["n"]
// CHECK: emitrust.member %{{.*}}["k"]

// A partially-designated global: the untouched slot runs fold to zeros
// (the degenerate binding carries no runtime information).
struct holder gh = { .n = 3, .k = 5 };
// CHECK: emitrust.global @gh <[3 : i32, [0, 0, 0, 0], [0, 0, 0, 0, 0, 0, 0, 0], [0, 0], 5 : i32]> : !emitrust.struct<"holder">

// LOAD-BEARING: a global initializer that names ADDRESSES folds those
// elements to literal 0 and records NO binding, so the emitted image is
// indistinguishable from the zero one. That is correct ONLY because every
// read of an element rejects (pointers-member-array-invalid.c's
// GLOBALREAD arm pins the read of exactly this global). Pinned here as an
// arm rather than left as an assumption.
int ga = 1, gb = 2;
struct bound { int *b[2]; int n; };
struct bound gbnd = { { &ga, &gb }, 7 };
// CHECK: emitrust.struct_def @bound ["b", "n"] [!emitrust.array<2xi64>, i32]
// CHECK: emitrust.global @gbnd <{{\[\[}}0, 0], 7 : i32]> : !emitrust.struct<"bound">

// Every local instance form. `{0}` and designated lists leave the slot
// runs implicit; an EXPLICIT all-null brace (`{0}` for the member itself)
// is accepted and emits nothing, which is the shape real declarations
// write. A non-null element there is rejected (see the invalid file's
// LOCALADDRINIT arm).
int locals(int v) {
  struct holder bare;
  bare.n = v;
  struct holder zero = {0};
  struct holder braced = {1, {0}, {0}, {0}, 3};
  struct holder desig = { .n = v, .k = 9 };
  struct holder table[2] = {{1, {0}, {0}, {0}, 2}, {3, {0}, {0}, {0}, 4}};
  struct holder copy = braced;
  copy.k += 1;
  return bare.n + zero.n + braced.k + desig.k + table[1].k + copy.k;
}
// CHECK-LABEL: func.func @locals
// CHECK: emitrust.variable named "bare" : !emitrust.lvalue<!emitrust.struct<"holder">>
// CHECK: emitrust.variable named "zero" : !emitrust.lvalue<!emitrust.struct<"holder">>
// CHECK: emitrust.variable named "braced" : !emitrust.lvalue<!emitrust.struct<"holder">>
//   The explicit `{0}` braces for m/p/s emit nothing at all: no subscript
//   op is created for a slot run, only the scalar members are stored.
// CHECK-NOT: emitrust.subscript
// CHECK: emitrust.variable named "desig" : !emitrust.lvalue<!emitrust.struct<"holder">>
// CHECK: emitrust.variable named "table" : !emitrust.lvalue<!emitrust.array<2x!emitrust.struct<"holder">>>
// CHECK: emitrust.variable named "copy" : !emitrust.lvalue<!emitrust.struct<"holder">>

// A record with a pointer-array member returns BY VALUE unchanged: the
// slot array is Copy, so the aggregate keeps its Copy semantics.
struct holder make(int v) {
  struct holder h = {0};
  h.k = v;
  return h;
}
int consume(int v) { struct holder h = make(v); return h.k; }
// CHECK-LABEL: func.func @make{{.*}} -> !emitrust.struct<"holder">
// CHECK-LABEL: func.func @consume

// The member nested inside another record, and inside an ANONYMOUS
// member: the slot run is an ordinary field in both flattened forms.
struct inner { struct T *ip[2]; int q; };
struct outer { struct inner in; int n; };
struct anon { int n; struct { void *am[3]; int k; }; };
int nestings(struct outer *o, struct anon *a) { return o->in.q + o->n + a->n + a->k; }
// CHECK: emitrust.struct_def @inner ["ip", "q"] [!emitrust.array<2xi64>, i32]
// CHECK: emitrust.struct_def @outer ["in_", "n"] [!emitrust.struct<"inner">, i32]
// CHECK: emitrust.struct_def @anon ["n", "am", "k"] [i32, !emitrust.array<3xi64>, i32]
