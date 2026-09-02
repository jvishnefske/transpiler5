// FR-128: a pass-1 method stub born INSIDE a failed item must not outlive
// the item's rollback -- an unresolvable reference is contained at the
// REFERENCING item, never crate-fatal under recovery.
//
// The defect this pins (a W2.25 regression, measured on all 8 spdlog units
// at fmt/bundled/format.h:1650): the FR-47 two-pass method walk registers
// every method signature as an external prototype before importing bodies;
// when a definition then reconciles the prototype away, FR-42 cloned it
// unconditionally, and on ITEM FAILURE `rollbackTo` re-materialized the
// clone -- a body-less stub of the just-failed item. Later admitted bodies
// (here: a W2.25 free operator's caller) found it via `functions.lookup`,
// bypassing the "call to an unimported constructor" containment, and died
// at finalize with the whole-crate `referenced but not defined` error -- no
// crate, no report, from ONE `__int128` sibling specialization. The fix
// captures a clone only for prototypes that PRE-DATE the item's checkpoint
// (`incremental-prototype-restore.cpp` pins that those still restore).
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --incremental %s \
// RUN:   -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/lib.rs
//
// Recovery OFF keeps the strict contract byte-for-byte: one located error
// at the first unsupported construct, non-zero exit, no output directory.
// RUN: not emitrust-cc --emit=crate --crate-type=lib %s \
// RUN:   -o %t.strict.crate 2>&1 | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate

// --- One template, two specializations. The u64 spec is fully importable;
// --- the __int128 spec fails on its field type, killing the WHOLE template
// --- item (all specs import under one recovery scope) after the u64 ctor
// --- was already defined -- the exact leak trigger.
template <typename F> struct BasicFp {
  // WARN: :[[#@LINE+1]]:5: warning: unsupported builtin type 'unsigned __int128' (recovered: item dropped)
  F f;
  int e;
  constexpr BasicFp() : f(0), e(0) {}
  constexpr BasicFp(unsigned long long f_val, int e_val) : f(f_val), e(e_val) {}
};
// WARN: :[[#@LINE+1]]:1: warning: unsupported top-level declaration (recovered: item dropped)
using Fp = BasicFp<unsigned long long>;

// --- W2.25-admitted free operator whose body constructs the leaked-stub
// --- type in value position: contained in ITS body, by its own channel.
inline auto operator*(Fp x, Fp y) -> Fp {
  // WARN: :[[#@LINE+1]]:10: warning: unsupported: constructor in value position (only a trivial copy or move is modeled) (recovered: emitted an unimplemented!() stub with the mapped signature)
  return {x.f * y.f, x.e + y.e + 64};
}

// --- Touches the failed sibling spec directly: rejected-type cascade.
inline int touch_other() {
  // WARN: :[[#@LINE+1]]:30: warning: unsupported: struct 'BasicFpX' was rejected, so a type naming it cannot be imported (recovered: emitted an unimplemented!() stub with the mapped signature)
  BasicFp<unsigned __int128> g;
  return g.e;
}

// --- The plain caller that used to be the crate-killer: its braced ctor
// --- call must hit the "unimported constructor" containment, because the
// --- ctor's pass-1 stub no longer survives the template item's rollback.
int use_it(int k) {
  // WARN: :[[#@LINE+1]]:6: warning: unsupported: call to an unimported constructor 'basic_fp_u64_ctor_ii' (recovered: emitted an unimplemented!() stub with the mapped signature)
  Fp a{2ULL, 1};
  Fp b{3ULL, k};
  Fp c = a * b;
  return c.e + touch_other();
}

// The run exits 0 with an honest per-item ledger -- and the finalize-time
// crate-killer never fires.
// WARN: recovered 6 rejected top-level items:
// WARN: stubbed 'op_mul' [other] unsupported: constructor in value position (only a trivial copy or move is modeled)
// WARN: stubbed 'touch_other' [rejected-type-cascade] unsupported: struct 'BasicFpX' was rejected, so a type naming it cannot be imported
// WARN: stubbed 'use_it' [other] unsupported: call to an unimported constructor 'basic_fp_u64_ctor_ii'
// WARN-NOT: referenced but not defined in any translation unit

// The u64 struct survives (FR-118 aggregate exemption), every referencing
// function is a located unimplemented!() stub, and no call to the erased
// ctor symbol reaches the crate.
// RUST: pub struct BasicFpU64 {
// RUST: pub fn op_mul(_v0: BasicFpU64, _v1: BasicFpU64) -> BasicFpU64 {
// RUST-NEXT: unimplemented!("unsupported: constructor in value position (only a trivial copy or move is modeled)")
// RUST: pub fn use_it(_v0: i32) -> i32 {
// RUST-NEXT: unimplemented!("unsupported: call to an unimported constructor 'basic_fp_u64_ctor_ii'")
// RUST-NOT: basic_fp_u64_ctor_ii(

// STRICT: error: unsupported builtin type 'unsigned __int128'
// STRICT-NOT: referenced but not defined
