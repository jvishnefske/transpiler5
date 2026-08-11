// REQUIRES: cargo
// W2.12: std::string_view over a string literal, end to end. Builds a
// Rust crate through the full import + conversion + Rust-emission +
// `cargo build --release` pipeline and diffs its stdout against a
// `clang++ -std=c++17` build of the identical source, byte for byte.
// Based on corpus 00905's shapes (size/remove_prefix/indexed char), with
// the remove_prefix amount derived from argc so constant folding cannot
// hide a miscompile in the cursor/len cell arithmetic: `full` must keep
// the PRE-mutation size (the ordering pin), the post-prefix size and the
// subscripts must track the dynamic cursor, and a second remove_prefix
// pins that the mutation composes. A second run with one extra argument
// shifts the prefix amount, so the two legs must agree on two distinct
// input vectors.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_string_view > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native extra > %t.native.out
// RUN: %t.crate/target/release/stl_string_view extra > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <string_view>

int main(int argc, char **argv) {
  std::string_view sv = "cpp17-corpus";
  int full = sv.size(); // 12, read BEFORE any mutation.
  sv.remove_prefix(argc * 3); // 3 on the bare run, 6 with one argument.
  // Bare run: "17-corpus" -> 12 9 1 7; extra: "corpus" -> 12 6 c r.
  printf("%d %d %c %c\n", full, (int)sv.size(), sv[0], sv[argc]);
  sv.remove_prefix(2);
  // Bare run: "-corpus" -> 7 -; extra: "rpus" -> 4 r.
  printf("%d %c\n", (int)sv.size(), sv[0]);
  return 0;
}
