// REQUIRES: cargo
// FR-113: branch merges with an ENUM result. A switch whose cases each
// return an enumerator (fmt's parse_align, the shape that kept every
// spdlog unit at 0/7) lifts through CF-to-SCF into an scf.index_switch
// with an !emitrust.enum result, and branchy early returns merge into an
// scf.if the same way; getDefaultValueAttr had no enum case, so BOTH
// shapes died as a NON-located `failed to legalize` that killed the whole
// unit -- for admitted unscoped enums too, pre-existing. The default is
// now the open enum's own `Name::default()` (every emitted enum_def
// carries an explicit `impl Default` returning its first variant, and the
// placeholder is dead: the SCF lowerings overwrite it on every branch).
// This leg byte-diffs both merge shapes, scoped and unscoped, with all
// selector values argc-derived so constant folding cannot hide the merge.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_enum_branchy > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

enum class Align { NONE, LEFT, RIGHT, CENTER };
enum Mode { OFF = 0, ON = 3, AUTO_M = 5 };

// The scf.index_switch shape: scoped enum returned from switch cases.
static Align parse_align(char c) {
  switch (c) {
  case '<': return Align::LEFT;
  case '>': return Align::RIGHT;
  case '^': return Align::CENTER;
  }
  return Align::NONE;
}

// The scf.if shape: unscoped enum, branchy early returns.
static Mode parse_mode(int n) {
  if (n > 4) return AUTO_M;
  if (n > 1) return ON;
  return OFF;
}

int main(int argc, char **argv) {
  printf("a %d\n", (int)parse_align((char)('<' + argc - 1)));
  printf("a2 %d\n", (int)parse_align((char)('^' + argc - 1)));
  printf("a3 %d\n", (int)parse_align((char)('x' + argc)));
  printf("m %d %d %d\n", (int)parse_mode(argc), (int)parse_mode(argc + 3),
         (int)parse_mode(argc - 1));
  return 0;
}
