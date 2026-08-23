// FR-125: a CamelCase namespace segment folds to snake_case in the
// emitted symbol prefix UNDER THE IDIOMATIC RENAME, and ONLY there.
//
// The defect this pins against: `namespacePrefix` kept the source
// capitalization (`namespace Game` -> `ns_Game_`), the emitted crate
// denies `non_snake_case`, and `cargo build` rejected every namespaced
// free function -- the transpiler exited 0 with an UNBUILDABLE crate
// (measured: 33 errors across 9 corpus crates). The fold is applied
// per-SEGMENT at the naming primitive (CSymbolNaming.h), exactly like
// `mangleMemberName`'s base-name fold, so it propagates identically to
// the importer, the FR-40 item graph, and the progress report -- and it
// is gated on `idiomaticRenameEnabled()`, so `emitrust-import-c` (and
// `--preserve-c-names`) keeps the verbatim C++ spelling byte-for-byte.
//
// Both legs of the gate are pinned here on the corpus shapes: a free
// function in a CamelCase namespace, a NESTED chain (per-segment fold:
// `Game::Input` -> `ns_game_ns_input_`), a multi-word segment
// (`PreGameSetup` -> `pre_game_setup`), and the case-fold INVARIANT
// spellings -- the record's UpperCamel fold and the internal-linkage
// const global's SCREAMING fold erased the segment's case before this
// fix, so `NsGameBox` / `TU0_NS_GAME_HIGH` must not shift.
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=PRESERVED
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RENAME

namespace Game {

const int high = 7;

struct Box {
  int v;
  int get() const { return v + 3; }
};

namespace Input {
int poll(int k) { return k * 2 + 1; }
} // namespace Input

int score(int s) { return s + Input::poll(s); }

} // namespace Game

namespace PreGameSetup {
int init(int n) { return n + Game::high; }
} // namespace PreGameSetup

int main() {
  Game::Box b;
  b.v = 4;
  return Game::score(1) + PreGameSetup::init(2) + b.get();
}

// PRESERVED-DAG: emitrust.struct_def @ns_Game_Box
// PRESERVED-DAG: func.func @ns_Game_Box_get(
// PRESERVED-DAG: func.func @ns_Game_ns_Input_poll(
// PRESERVED-DAG: func.func @ns_Game_score(
// PRESERVED-DAG: func.func @ns_PreGameSetup_init(

// RENAME-DAG: struct NsGameBox {
// RENAME-DAG: static TU0_NS_GAME_HIGH: i32 = 7;
// RENAME-DAG: fn get(
// RENAME-DAG: fn ns_game_ns_input_poll(
// RENAME-DAG: fn ns_game_score(
// RENAME-DAG: fn ns_pre_game_setup_init(
