// FR-128 companion: the two LEGITIMATE uses of the FR-42 clone-restore
// must survive the fix that stopped restoring intra-item pass-1 stubs
// (incremental-rollback-stub-containment.cpp). The invariant: a prototype
// that PRE-DATES the failing item's checkpoint is still cloned and
// restored on rollback, so a call already imported against it keeps its
// callee while the failed definition stub-retries into a located
// unimplemented!() body.
//
// Shape A (cross-item prototype): `callf` imports its call against the
// prototype of `f`; f's DEFINITION item then fails; the restored prototype
// is what lets the stub retry give `f` a body `callf` can still name.
// Shape B (out-of-line method of a SUCCEEDED class): the pass-1 stub for
// `S::get` is born in the CLASS item -- a PREVIOUS checkpoint -- so the
// failing out-of-line definition item must restore it, and the method
// stub lands inside `impl S` where `use_` resolves it.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --incremental %s \
// RUN:   -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/lib.rs
//
// Strict mode is untouched: located error, non-zero exit, no output.
// RUN: not emitrust-cc --emit=crate --crate-type=lib %s \
// RUN:   -o %t.strict.crate 2>&1 | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate

// --- Shape A: prototype, a call bound to it, then the failing definition.
int f(int);
int callf(int x) { return f(x); }
// WARN: :[[#@LINE+1]]:34: warning: unsupported builtin type 'unsigned __int128' (recovered: emitted an unimplemented!() stub with the mapped signature)
int f(int x) { unsigned __int128 big = (unsigned __int128)x; return (int)(big >> 64) + x; }
int g(int x) { return callf(x) + 1; }

// --- Shape B: in-class declaration (pass-1 stub in the class's own item),
// --- failing out-of-line definition, and a caller that must still resolve.
struct S {
  int v;
  int get() const;
};
// WARN: :[[#@LINE+1]]:40: warning: unsupported builtin type 'unsigned __int128' (recovered: emitted an unimplemented!() stub with the mapped signature)
int S::get() const { unsigned __int128 x = (unsigned __int128)v; return (int)(x >> 64) + v; }
int use_it(S s) { return s.get(); }

// Both retries land in the ledger as stubs -- nothing is crate-fatal.
// WARN: recovered 2 rejected top-level items:
// WARN: stubbed 'f' [other] unsupported builtin type 'unsigned __int128'
// WARN: stubbed 's_get' [other] unsupported builtin type 'unsigned __int128'
// WARN-NOT: referenced but not defined in any translation unit

// The callers keep their callees; the failed definitions are located stubs.
// RUST-DAG: pub fn callf(x: i32) -> i32 {
// RUST-DAG: pub fn f(_v0: i32) -> i32 {
// RUST-DAG: unimplemented!("unsupported builtin type 'unsigned __int128'")
// RUST-DAG: pub fn g(x: i32) -> i32 {
// RUST-DAG: pub struct S {
// RUST-DAG: pub fn get(&self) -> i32 {
// RUST-DAG: s.get()

// STRICT: error: unsupported builtin type 'unsigned __int128'
// STRICT-NOT: referenced but not defined
