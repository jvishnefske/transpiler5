// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST
// W2.24: `try`/`throw`/`catch` as Result threading through ONE synthesized
// closed data enum per thrown payload type per TU (`Throws_i32`;
// `ThrowsI32` under the idiomatic rename — the same non_camel_case_types
// deny constraint W2.14 measured). Pins the chosen lowerings at the IR
// level, because every one of them is load-bearing:
//  - the carrier is a module-level `emitrust.data_enum_def` with variants
//    `Ok0`/`Err0` (a pinned FREE choice: W2.14's precedent spells V0/V1,
//    and nothing may drift between the def, the enum_variant constructs
//    and the match arms) each carrying one payload field `v` — the wave-1
//    gate proved the declared return type IS the thrown payload type, so
//    both variants share one payload type and a call site can unwrap with
//    a SINGLE payload match;
//  - a function in the can-throw closure (INCLUDING a namespaced one: the
//    planner's recursive walk mirrors importDeclsIn, NOT the TU-only
//    collectPassAFunctionDefinitions scaffold whose miss was the spike's
//    measured silent-drop trap) returns the carrier; `throw` constructs
//    `Err0` and returns it, `return` wraps its value in `Ok0`;
//  - a call to a closure member unwraps through TWO RESULT-mode matches
//    (an i1 discriminant and the shared payload) plus the early-return
//    cf pattern. `emitrust.return` is HasParent<FuncOp>, so
//    `Err(e) => return Err(e)` is UNSPELLABLE inside a match arm, and the
//    single-slot statement-mode spelling is denied by unused_assignments
//    (both measured, design.md W2.24) — the two-match + cf.cond_br shape
//    is the only verified route, structurized by lift-cf-to-scf into the
//    existing if/else-plus-tail pyramid;
//  - a handler delivers through the SAME cf pattern into the catch block,
//    with the caught payload living in an ordinary variable place bound
//    to the catch parameter.
// test/EndToEnd/cpp-exceptions.cpp byte-diffs the behavior; the frontier
// around this subset is pinned in exceptions-invalid.cpp.

extern "C" int printf(const char *, ...);

namespace deep {
int inner(int x) {
  if (x == 0)
    throw 42;
  return 100 / x;
}
} // namespace deep

int outer(int x) { return deep::inner(x) + 1; }

int main() {
  int caught = 0;
  int ok = 0;
  try {
    ok = outer(4);
    outer(0);
  } catch (int e) {
    caught = e;
  }
  printf("%d %d\n", ok, caught);
  return 0;
}

// The synthesized carrier: one module-level def, Ok0/Err0, payload "v".
// CHECK: emitrust.data_enum_def @Throws_i32 ["Ok0", "Err0"] {{\[\["v"\], \["v"\]\] \[\[i32\], \[i32\]\]}}

// The namespaced thrower joins the closure and returns the carrier.
// CHECK-LABEL: func.func @ns_deep_inner(
// CHECK-SAME: -> !emitrust.data_enum<"Throws_i32">
// throw 42 -> construct Err0 + return (the early-return pyramid's seed).
// CHECK: %[[ERR:.*]] = emitrust.enum_variant @Throws_i32 "Err0"(%{{.*}}) : (i32) -> !emitrust.data_enum<"Throws_i32">
// CHECK: return %[[ERR]] : !emitrust.data_enum<"Throws_i32">
// return 100/x -> wrap in Ok0.
// CHECK: %[[OK:.*]] = emitrust.enum_variant @Throws_i32 "Ok0"(%{{.*}}) : (i32) -> !emitrust.data_enum<"Throws_i32">
// CHECK: return %[[OK]] : !emitrust.data_enum<"Throws_i32">

// The propagating caller: in the closure because of its unprotected call
// edge, carrier signature, and the two-match unwrap at the call site.
// CHECK-LABEL: func.func @outer(
// CHECK-SAME: -> !emitrust.data_enum<"Throws_i32">
// CHECK: %[[R:.*]] = call @ns_deep_inner(%{{.*}}) : (i32) -> !emitrust.data_enum<"Throws_i32">
// CHECK: %[[DISC:.*]] = emitrust.match %[[R]] : !emitrust.data_enum<"Throws_i32"> -> i1
// CHECK: case "Ok0" (%{{.*}}: i32)
// CHECK: case "Err0" (%{{.*}}: i32)
// CHECK: %[[PAY:.*]] = emitrust.match %[[R]] : !emitrust.data_enum<"Throws_i32"> -> i32
// CHECK: case "Ok0" (%[[POK:.*]]: i32)
// CHECK: emitrust.yield %[[POK]] : i32
// CHECK: case "Err0" (%[[PERR:.*]]: i32)
// CHECK: emitrust.yield %[[PERR]] : i32
// CHECK: cf.cond_br %[[DISC]]
// The propagation: re-wrap the payload and return it to THIS caller's
// caller (never a catch: outer has no try).
// CHECK: emitrust.enum_variant @Throws_i32 "Err0"(%[[PAY]])

// main keeps its i32 signature (it can never join the closure); the
// handler delivers through the same cf pattern into the catch block,
// assigning the bound catch variable's place.
// CHECK-LABEL: func.func @c_main(
// CHECK-SAME: -> i32
// CHECK: %[[E:.*]] = emitrust.variable named "e" : !emitrust.lvalue<i32>
// CHECK: call @outer
// CHECK: emitrust.match
// CHECK: cf.cond_br
// CHECK: emitrust.assign %[[E]] = %{{.*}}

// The crate-level camel pin: the carrier must survive the
// non_camel_case_types deny, and data enums deliberately derive no
// Default (a surviving placeholder is a loud E0599, never a value).
// RUST: enum ThrowsI32
// RUST: Ok0 { v: i32 }
// RUST: Err0 { v: i32 }
// RUST: fn ns_deep_inner({{.*}}) -> ThrowsI32
// RUST: fn outer({{.*}}) -> ThrowsI32
