// REQUIRES: cargo
// FR-231: where the per-TU internal-linkage tag goes once a namespace becomes
// a PATH -- pinned as a spelling and then as behavior.
//
// `cFunctionSymbolName` has always composed the tag OUTSIDE the namespace
// prefix: `joinSymbolPrefix(tuTag, base)` over an already-prefixed base gives
// `tu0_ns_ns_helper`. Applied unchanged to a path that is
// `tu0_crate::ns::helper` -- not a Rust path at all, and the same composition
// sits in the global-variable namer just below it. So under
// `--namespace-modules` the tag moves to the LEAF, `crate::ns::tu0_helper`,
// where it still does the only job it has: keeping two translation units'
// identically named file-statics distinct. (The overload and template
// suffixes needed no move; they already appended to the leaf.)
//
// Two TUs each define `namespace ns { static int helper(..); static int bias; }`
// with DIFFERENT bodies and DIFFERENT initializers, and each is reached
// through its own external-linkage entry point. If the tag were dropped, or
// landed somewhere that let the two compose onto one symbol, the two TUs'
// statics would merge -- and the merge is OBSERVABLE here rather than merely
// unbuildable: both results are printed, both derive from argc, and the `* 100`
// separates the two helpers' contributions digit-wise so a substitution cannot
// coincide.
//
// The MOD leg pins the emitted spelling on both sides of the flag (the
// flagless spelling `tu0_ns_ns_helper` is the historical one, byte for byte);
// the diff legs are the oracle, because a symbol collision that rustc happens
// to accept is exactly what compile-only evidence cannot see.
//
// RUN: split-file %s %t
// RUN: emitrust-cc --emit=rust --namespace-modules %t/a.cpp %t/b.cpp %t/main.cpp \
// RUN:   | FileCheck %s --check-prefix=MOD
// RUN: emitrust-cc --emit=rust %t/a.cpp %t/b.cpp %t/main.cpp \
// RUN:   | FileCheck %s --check-prefix=FLAT
// RUN: emitrust-cc --emit=crate --namespace-modules %t/a.cpp %t/b.cpp %t/main.cpp \
// RUN:   -o %t.mod --crate-name nsmod_tu_on --build
// RUN: emitrust-cc --emit=crate %t/a.cpp %t/b.cpp %t/main.cpp \
// RUN:   -o %t.flat --crate-name nsmod_tu_off --build
// RUN: clang++ -std=c++17 %t/a.cpp %t/b.cpp %t/main.cpp -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.mod/target/release/nsmod_tu_on > %t.mod.out
// RUN: %t.flat/target/release/nsmod_tu_off > %t.flat.out
// RUN: diff %t.native.out %t.mod.out
// RUN: diff %t.native.out %t.flat.out

//--- a.cpp
extern "C" int printf(const char *, ...);
namespace ns {
static int helper(int v) { return v + 1; }
static int bias = 5;
int from_a(int v) { return helper(v) * 100 + bias; }
} // namespace ns

//--- b.cpp
namespace ns {
static int helper(int v) { return v * 7; }
static int bias = 9;
int from_b(int v) { return helper(v) * 100 + bias; }
} // namespace ns

//--- main.cpp
extern "C" int printf(const char *, ...);
namespace ns {
int from_a(int);
int from_b(int);
} // namespace ns
int main(int argc, char **argv) {
  printf("%d %d\n", ns::from_a(argc), ns::from_b(argc));
  return 0;
}

// The tag rides the LEAF, inside one shared `mod ns`.
// MOD:      mod ns {
// MOD-NEXT:     pub(crate) fn tu0_helper(v: i32) -> i32 {
// MOD:          pub(crate) fn tu1_helper(v: i32) -> i32 {
// MOD-NOT:  tu0_crate

// The default keeps the historical outside-the-prefix composition.
// FLAT:     fn tu0_ns_ns_helper(v: i32) -> i32 {
// FLAT:     fn tu1_ns_ns_helper(v: i32) -> i32 {
