// REQUIRES: cargo
// FR-231: what a C++ namespace NAME becomes when it turns into a Rust `mod`,
// and the lint cover the answer needs.
//
// The segment casing MIRRORS `namespacePrefix`, deliberately: FR-125 folds
// each flattened segment to snake_case under the idiomatic rename
// (`namespace Game` -> `ns_game_`) and keeps the C++ spelling verbatim under
// `--preserve-c-names`, and a module path that disagreed with the flattening
// on the same input would be a second naming rule to keep in step for no
// gain. So `Game::Input::Poll` is `crate::game::input::poll` by default and
// `crate::Game::Input::Poll` under `--preserve-c-names`.
//
// THE LINT COVER, which is not decorative. The emitted crate DENIES
// `non_snake_case` in its own Cargo.toml `[lints.rust]` block, and a `mod`
// name is subject to it. Two shapes reach it:
//   * a verbatim CamelCase segment under `--preserve-c-names`;
//   * a DOUBLED UNDERSCORE, which survives the idiomatic rename untouched --
//     `toSnakeCase` never doubles or collapses an underscore, so
//     `namespace a__b` is `mod a__b` in BOTH modes. This is the leg that
//     matters: `--preserve-c-names` already emits a crate-level
//     `#![allow(non_snake_case, ..)]`, so it would have covered its own case
//     anyway, while the default rename mode has no such blanket and the
//     per-item attribute is the only thing standing between this input and
//     `error: module `a__b` should have a snake case name` (MEASURED: deleting
//     the attribute from the emitted crate fails the build with exactly that).
//
// The BUILD run is what proves the cover sufficient rather than plausible;
// `--emit=rust` alone cannot see a denied lint.
//
// RUN: emitrust-cc --emit=rust --namespace-modules %s | FileCheck %s --check-prefix=RENAME
// RUN: emitrust-cc --emit=rust --preserve-c-names --namespace-modules %s \
// RUN:   | FileCheck %s --check-prefix=VERBATIM
// RUN: emitrust-cc --emit=crate --build --namespace-modules %s -o %t.crate

extern "C" int printf(const char *, ...);

namespace Game {
namespace Input {
int Poll(int v) { return v + 1; }
} // namespace Input
} // namespace Game

namespace a__b {
int f(int v) { return v * 2; }
} // namespace a__b

int main(int argc, char **argv) {
  printf("%d %d\n", Game::Input::Poll(argc), a__b::f(argc));
  return 0;
}

// RENAME:          let v1: i32 = crate::game::input::poll(argc);
// RENAME-NEXT:     let v2: i32 = crate::a__b::f(argc);
// The snake-folded segments need no cover...
// RENAME:      mod game {
// RENAME-NEXT:     pub(crate) mod input {
// RENAME-NEXT:         pub(crate) fn poll(v: i32) -> i32 {
// ...the doubled underscore, which the fold cannot remove, does.
// RENAME:      #[allow(non_snake_case)]
// RENAME-NEXT: mod a__b {
// RENAME-NEXT:     pub(crate) fn f(v: i32) -> i32 {

// VERBATIM:        let v1: i32 = crate::Game::Input::Poll(argc);
// VERBATIM:    #[allow(non_snake_case)]
// VERBATIM-NEXT: mod Game {
// VERBATIM-NEXT:     #[allow(non_snake_case)]
// VERBATIM-NEXT:     pub(crate) mod Input {
// VERBATIM-NEXT:         pub(crate) fn Poll(v: i32) -> i32 {
