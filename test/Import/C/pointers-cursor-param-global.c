// RUN: emitrust-import-c %s | FileCheck %s

// C99-43 front C1 (Shape G): the single-global-or-NULL out-param cursor.
//
// Pins the invariant that a `T **p` cursor parameter in an
// otherwise-admissible cursor-param function, whose single unconditional
// top-level write `*p = <rhs>` has an RHS inside the C1 grammar — one
// statically-known whole global `g` (array or scalar), `NULL`/`0`, or
// `cond ? g : NULL` — lowers to ONE `&mut Option<i64>` in-out cell
// (Q1: arity preserved, no multi-return; Q4: Option-of-cursor NULL,
// None = C NULL, Some(offset) = element offset into g's backing; Q2's
// "needs a second in-out state cell" objection is obsoleted by the
// Option cell folding the NULL flag and the offset into one value —
// the synthesized-region prohibition stands and keeps multi-global
// rejected, see pointers-cursor-param-global-invalid.c). The callee's
// write becomes a Some/None assignment through the deref'd reference;
// the caller stages a `Option<i64>` temp (init None), passes `&mut`,
// and after the call destructures it back into the pointer local's
// existing CTS-P8/P6 cells (non-null flag via `.is_some()`, cursor via
// `.unwrap_or(0)`; a never-null param reads the offset back with
// `.expect("null pointer read")`), so every later use of the pointer —
// `if (p)`, `p[i]`, `*p` — reuses the decomposed-pointer machinery
// unchanged. The runtime observable is pinned differentially in
// test/EndToEnd/cursor-param-global-or-null.c.

int printf(const char *, ...);

unsigned err_flags[4] = {1u, 2u, 4u, 8u};
unsigned tab[3] = {9u, 10u, 11u};
int counter = 7;

// (1) The check_cpu family: `cond ? g : NULL` over an array global.
int check(int *lvl, unsigned **efp) {
  int err = *lvl > 2;
  *lvl = *lvl + 10;
  *efp = err ? err_flags : 0;
  return err;
}
// CHECK-LABEL: func.func @check
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.mut_ref<i32>, %{{[^,)]+}}: !emitrust.mut_ref<!emitrust.opaque<"Option<i64>">>) -> i32
// CHECK-SAME: emitrust.param_names = ["lvl", "efp"]
// The cell is deref'd once; the ternary write assigns Some(0)/None
// per arm.
// CHECK: %[[CELL:.+]] = emitrust.deref %arg1 : (!emitrust.mut_ref<!emitrust.opaque<"Option<i64>">>) -> !emitrust.lvalue<!emitrust.opaque<"Option<i64>">>
// CHECK: %[[SOME:.+]] = emitrust.literal "Some(0i64)" : !emitrust.opaque<"Option<i64>">
// CHECK: emitrust.assign %[[CELL]] = %[[SOME]]
// CHECK: %[[NONE:.+]] = emitrust.literal "None" : !emitrust.opaque<"Option<i64>">
// CHECK: emitrust.assign %[[CELL]] = %[[NONE]]

// (2) Unconditional whole-array global: always Some(0). Targets its
// OWN global so the caller-side pointer's region stays never-null
// (regions unify through shared bases; a shared target would inherit
// ef's nullability below and hide the expect path).
void point_at(unsigned **out) {
  *out = tab;
}
// CHECK-LABEL: func.func @point_at
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.mut_ref<!emitrust.opaque<"Option<i64>">>)
// CHECK: %[[CELL2:.+]] = emitrust.deref %arg0
// CHECK: %[[SOME2:.+]] = emitrust.literal "Some(0i64)" : !emitrust.opaque<"Option<i64>">
// CHECK: emitrust.assign %[[CELL2]] = %[[SOME2]]

// (3) Scalar global through `cond ? &g : 0` (degenerate base: the
// caller-side pointer carries no cursor, only the Option discriminant).
int grab(int want, int **out) {
  *out = want ? &counter : 0;
  return want;
}
// CHECK-LABEL: func.func @grab
// CHECK-SAME: (%{{[^,)]+}}: i32, %{{[^,)]+}}: !emitrust.mut_ref<!emitrust.opaque<"Option<i64>">>) -> i32

// (4) Pure NULL: the degenerate empty-global-set grammar point.
void clear_it(unsigned **out) {
  *out = 0;
}
// CHECK-LABEL: func.func @clear_it
// CHECK-SAME: (%{{[^,)]+}}: !emitrust.mut_ref<!emitrust.opaque<"Option<i64>">>)
// CHECK: %[[CELL4:.+]] = emitrust.deref %arg0
// CHECK: %[[NONE4:.+]] = emitrust.literal "None" : !emitrust.opaque<"Option<i64>">
// CHECK: emitrust.assign %[[CELL4]] = %[[NONE4]]

// Caller flow: a local `unsigned *ef` fed through `&ef` stages an
// Option temp, and the pointer's later uses (`if (ef)`, `ef[i]`)
// resolve against the plan's global via the staged-copy machinery.
int main(void) {
  int lvl = 5;
  unsigned *ef;
  int r = check(&lvl, &ef);
  unsigned first = 0u;
  if (ef)
    first = ef[1];
  unsigned *pinned;
  point_at(&pinned);
  unsigned always = pinned[2];
  int *c;
  grab(1, &c);
  int got = c ? *c : -1;
  clear_it(&ef);
  int gone = ef ? 1 : 0;
  return (int)(first + always) + got + gone + r;
}
// CHECK-LABEL: func.func @c_main
// The staged out-cell temp starts as None...
// CHECK: %[[TMP:.+]] = emitrust.variable : !emitrust.lvalue<!emitrust.opaque<"Option<i64>">>
// CHECK: %[[INIT:.+]] = emitrust.literal "None" : !emitrust.opaque<"Option<i64>">
// CHECK: emitrust.assign %[[TMP]] = %[[INIT]]
// CHECK: %[[REF:.+]] = emitrust.addr_of mut %[[TMP]] : (!emitrust.lvalue<!emitrust.opaque<"Option<i64>">>) -> !emitrust.mut_ref<!emitrust.opaque<"Option<i64>">>
// CHECK: call @check(%{{.+}}, %[[REF]])
// ...and after the call the Option destructures back into ef's
// (non-null flag, cursor) cells: nullable path via is_some/unwrap_or.
// CHECK: %[[FLAG:.+]] = emitrust.method_call %[[TMP]]["is_some"] () : (!emitrust.lvalue<!emitrust.opaque<"Option<i64>">>) -> i1
// CHECK: memref.store %[[FLAG]]
// CHECK: %[[ZERO:.+]] = arith.constant 0 : i64
// CHECK: %[[CUR:.+]] = emitrust.method_call %[[TMP]]["unwrap_or"] (%[[ZERO]]) : (!emitrust.lvalue<!emitrust.opaque<"Option<i64>">>, i64) -> i64
// CHECK: memref.store %[[CUR]]
// The never-null point_at target reads its offset back with the Q3
// deterministic-panic spelling (unreachable: the callee always writes
// Some).
// CHECK: call @point_at(
// CHECK: %[[MSG:.+]] = emitrust.literal "\22null pointer read\22" : !emitrust.opaque<"&str">
// CHECK: emitrust.method_call %{{.+}}["expect"] (%[[MSG]]) : (!emitrust.lvalue<!emitrust.opaque<"Option<i64>">>, !emitrust.opaque<"&str">) -> i64
// The scalar-global target has no cursor cell: only the flag comes back.
// CHECK: call @grab(
// CHECK: emitrust.method_call %{{.+}}["is_some"] ()
// CHECK: call @clear_it(
