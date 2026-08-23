// REQUIRES: cargo
// FR-125: a CamelCase namespace segment folds to snake_case in the
// emitted symbol prefix (`namespace Game` -> `ns_game_`), so the crate
// builds under its own denied `non_snake_case` lint. Before this fix the
// prefix kept the source capitalization (`ns_Game_score`), the
// transpiler exited 0, and `cargo build` hard-failed on the lint -- an
// exit-0 UNBUILDABLE crate (measured: 33 errors across 9 corpus crates;
// 2048.cpp's `namespace Game`, jsoncpp's `namespace Json`).
//
// Pins the corpus shapes end to end: a free function in a CamelCase
// namespace, a NESTED CamelCase chain (`Game::Input` ->
// `ns_game_ns_input_`, the fold is per-segment), a multi-word segment
// (`PreGameSetup` -> `pre_game_setup`), a file-static function inside
// the namespace (tuTag composes in front of the folded prefix), a
// record whose emitted spelling is case-fold INVARIANT (`NsGameBox` --
// the UpperCamel fold already erased the segment's case), and a WRITTEN
// namespace global (lowered through the mutable-global machinery, every
// derived spelling snake-clean). Every value derives from argc so constant
// folding cannot pre-compute the answers and hide a miscompile behind a
// compile-clean crate. Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_namespace_camelcase > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

namespace Game {

int high = 0;

struct Box {
  int v;
  int get() const { return v + 3; }
};

namespace Input {
int poll(int k) { return k * 2 + 1; }
} // namespace Input

static int clamp(int s) { return s > 9 ? 9 : s; }

int score(int s) { return clamp(s) + Input::poll(s); }

int readBack() { return high - 2; }

} // namespace Game

namespace PreGameSetup {
int init(int n) { return n + 40; }
} // namespace PreGameSetup

int main(int argc, char **argv) {
  int seed = argc; /* 1 at run time, opaque to the folder */
  Game::high = Game::score(seed) + PreGameSetup::init(seed);
  Game::Box b;
  b.v = seed * 5;
  printf("%d %d %d %d\n", Game::readBack(), b.get(),
         Game::Input::poll(seed + 6), Game::high);
  return 0;
}
