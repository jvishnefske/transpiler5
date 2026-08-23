// FR-116 DEFECT regression, manifestation 1 (the three-line repro): a
// non-const global READ FROM A C++ METHOD BODY used to break the crate
// outright. `emitrust.impl` carries the MLIR SymbolTable trait and
// `SymbolTable::getSymbolUses` does not traverse into nested symbol tables,
// so the actor plan (built from the FR-40 item graph, which deliberately
// skips every CXXMethodDecl) saw no access at all, localized the global
// into c_main, and the pass then erased it under the method's live
// reference -- the MLIR verifier, not the pass, reported the bug:
//   error: 'emitrust.global_load' op 'G' does not reference a valid
//   emitrust.global
// The fix is an impl-aware use scan in the pass. THIS IS THE "the global is
// NOT actor-lifted" PIN the conservative shape owes: the located demotion
// warning fires, and the emitted Rust keeps the proven-correct thread-local
// Cell form with the method reading it -- byte-diffed against clang++ in
// test/EndToEnd/cpp-method-global.cpp. The cost, stated plainly: any global
// touched from a method body never becomes an actor field.
// RUN: emitrust-cc --emit=rust %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --check-prefix=WARN < %t.err
// RUN: FileCheck %s --check-prefix=RUST --implicit-check-not=GActor \
// RUN:   --implicit-check-not=Actor < %t.rs
//
// WARN: warning: actor lift: demoted g: global 'G' has a use outside the driver
//
// RUST: thread_local!
// RUST: static G: std::cell::Cell<i32>
// RUST: fn peek(&mut self) -> i32
// RUST: G.with(

extern "C" int printf(const char *, ...);

int g = 7;

struct S {
  int v;
  S(int x) : v(x) {}
  int peek() { return g + v; }
};

int main(int argc, char **) {
  S s(argc);
  printf("%d\n", s.peek());
  return 0;
}
