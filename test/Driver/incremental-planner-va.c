// FR-53, the SOUNDNESS half: the va_list monomorphization planner
// (`planVaMonomorph`) builds cross-declaration state -- a per-target clone list
// and a per-call-site clone index -- so recovering one of its rejections is not
// just a matter of skipping a declaration. Two facts have to hold, and this
// test pins both.
//
// 1. Dropping a variadic DEFINITION must un-plan it entirely. Its clones are
//    the only symbols its calls would name (a monomorphized definition never
//    keeps a plain symbol), so a surviving caller that kept its clone index
//    would emit a call to a function that was never built -- a dangling symbol,
//    which is a miscompile, strictly worse than the hard failure this replaces.
//    The rejected definition is therefore removed from the target set and the
//    plan is REBUILT without it, which makes every caller take the ordinary
//    "call to a variadic function" rejection and be recovered in turn.
//
// 2. Dropping a CALLER must not un-plan the callee. A rejection whose cause is
//    a property of the call site (here: taking the definition's address) is
//    attributed to the declaration that wrote it, and the variadic definition
//    stays monomorphized for its remaining call sites.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// `--implicit-check-not` is what proves fact 1 over the WHOLE file: no clone
// of `pick`, under any name, anywhere in the crate.
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/main.rs \
// RUN:   --implicit-check-not=tu0_pick
//
// Recovery off keeps the first rejection, as an error, with no crate.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict.crate 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate

#include <stdarg.h>

// Case 1: `va_copy` is a fact about this definition. `pick` is dropped, so its
// caller `use_pick` loses the callee and is stubbed rather than left calling a
// clone that does not exist.
static int pick(int n, ...) {
  va_list ap, copy;
  va_start(ap, n);
  // WARN: :[[#@LINE+1]]:3: warning: unsupported: va_copy (recovered: item dropped)
  va_copy(copy, ap);
  int value = va_arg(copy, int);
  va_end(copy);
  va_end(ap);
  return value;
}

// WARN: :[[#@LINE+1]]:29: warning: unsupported: call to a variadic function (recovered: emitted an unimplemented!() stub with the mapped signature)
int use_pick(void) { return pick(1, 7); }

// Case 2: `sum` is in the subset. Only `taker` does something unsupported with
// it, so only `taker` is rejected.
static int sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int value = va_arg(ap, int);
  va_end(ap);
  return value;
}

typedef int (*any_fn)();

// WARN: :[[#@LINE+1]]:44: warning: unsupported: address of variadic definition (recovered: emitted an unimplemented!() stub with the mapped signature)
static any_fn taker(void) { return (any_fn)sum; }

int main(void) { return sum(1, 2) + use_pick(); }

// The caller's rejection is the ORDINARY call-site one, raised by `emitCall`
// because the callee has no plan -- not a second recovery mechanism invented
// for planner failures. That is the whole propagation argument: an un-planned
// variadic definition is indistinguishable from one the importer never
// supported, so the existing rejection covers every caller for free.
// WARN: recovered 3 rejected top-level items:
// WARN: dropped 'pick' [other] unsupported: va_copy
// WARN: stubbed 'use_pick' [variadic-cross-tu] unsupported: call to a variadic function
// WARN: stubbed 'tu0_taker' [other] unsupported: address of variadic definition

// `sum` still monomorphizes -- one clone for its one `(int)` call site -- and
// `main` still calls it directly. `pick` is gone, clone and all, and nothing
// references it.
// RUST: fn use_pick() -> i32 {
// RUST-NEXT: unimplemented!("unsupported: call to a variadic function")
// RUST: fn tu0_sum_1(_v0: i32, v1: i32) -> i32 {
// RUST: fn tu0_taker() -> Option<fn() -> i32> {
// RUST-NEXT: unimplemented!("unsupported: address of variadic definition")
// RUST: fn c_main() -> i32 {
// RUST: tu0_sum_1(

// STRICT: error: unsupported: va_copy
