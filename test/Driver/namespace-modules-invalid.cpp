// FR-231: the two ways `--namespace-modules` interacts with the crate's
// EXPORT surface, one refused at the command line and one pinned as a
// documented consequence. Neither may change silently -- an export that
// disappears is discovered at link time in somebody else's harness, with no
// diagnostic and nothing in the artifact to explain it.
//
// THE REFUSAL. The Rust emitter's C-ABI export loop skips every item carrying
// a module path, and it has always been right to: FR-159 minted those paths
// for per-TU modules, whose contents are translation-unit-local and have no C
// symbol to give. Under `--namespace-modules` a module path no longer implies
// TU-local, so that same skip would silently DROP the export of a namespaced
// `extern "C"` function. Refused at the driver rather than reinterpreted:
// teaching the export loop to tell the two kinds of module path apart is a
// real feature with its own unanswered question (a `#[no_mangle]` item inside
// a `mod` IS exported, but its C name would then be minted by a flag the C
// caller never saw), and shipping that as a side effect of a NAMING flag is
// how a wrong number gets built later. The refusal is checked BEFORE
// `--c-abi-exports`'s own crate-shape and emit-mode validation, so the
// diagnostic names the real conflict rather than whichever other precondition
// happens to fail first -- the same ordering `--partition` takes.
//
// THE CONSEQUENCE. Without `--c-abi-exports`, an ordinary `--crate-type=lib`
// build is accepted, and a namespaced item is `pub(crate)` INSIDE its module
// while a root item stays `pub`. That is not a bug and not a refusal: it is
// FR-51's export decision meeting FR-159's fixed module visibility, and it
// means a library whose public surface lives in a namespace has no public
// surface under this flag. Pinned here so the trade is written down where a
// user hits it.
//
// RUN: not emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   --namespace-modules %s -o %t.x 2>&1 | FileCheck %s --check-prefix=CABI
//
// The refusal fires ahead of the emit-mode check too, not after it.
// RUN: not emitrust-cc --emit=mlir --c-abi-exports --namespace-modules %s \
// RUN:   -o /dev/null 2>&1 | FileCheck %s --check-prefix=CABI
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --namespace-modules %s \
// RUN:   -o %t.lib
// RUN: cat %t.lib/src/lib.rs | FileCheck %s --check-prefix=LIB

namespace geo {
int twice(int v) { return v * 2; }
} // namespace geo

int root_twice(int v) { return geo::twice(v); }

// CABI: error: --c-abi-exports does not apply under --namespace-modules: an item inside a Rust module is not exported, so a namespaced function would silently lose its C symbol

// LIB:      pub fn root_twice(v: i32) -> i32 {
// LIB-NEXT:     crate::geo::twice(v)
// LIB:      mod geo {
// LIB-NEXT:     pub(crate) fn twice(v: i32) -> i32 {
