// REQUIRES: cargo
// FR-202: `s += s` on a std::string, end to end. C++ defines the aliased
// append exactly (`basic_string::append` reads its operand before it
// grows, so `"ab" += "ab"` is `"abab"`), but the STL `operator+=` arm had
// NO borrow-collision check at all and emitted `s.push_str(&s)` — a crate
// `emitrust-cc` exited 0 on and rustc then refused with E0502. Compile
// success proves nothing here: the shape BUILT clean through the
// transpiler and died only in cargo, so the invariant this file pins is
// the runtime one — the built crate's stdout must byte-match the clang++
// native for BOTH the aliased and the non-aliased right-hand sides.
//
// The negative controls (`u += t`, `u += "lit"`, `u += char`) are in the
// same file on purpose: the fix stages a CLONE only when the right-hand
// side shares a root with the receiver, so a regression that staged
// unconditionally would still pass a diff — but it would shift the bytes
// of the goldens in test/Import/Cpp, which is the other half of the pin.
// Every value derives from argc, and the test runs twice (argc=1 and
// argc=2 via the `seed` argument), so no leg can be constant-folded.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_string_self_append > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native seed > %t.native2.out
// RUN: %t.crate/target/release/stl_string_self_append seed > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out

extern "C" int printf(const char *, ...);

#include <string>

int main(int argc, char **) {
  std::string s = "ab";
  if (argc > 1)
    s += "Q";
  // The aliasing append, twice: a staging bug that only worked once (or
  // that read the receiver AFTER it grew) shows up as a length mismatch.
  s += s;
  s += s;
  printf("%s %d\n", s.c_str(), (int)s.size());

  // Non-aliasing right-hand sides: these must keep the plain
  // `push_str(&t)` lowering and are here as the over-staging control.
  std::string t = "xy";
  if (argc > 1)
    t += "Z";
  std::string u;
  u += t;
  u += "lit";
  u += (char)('a' + argc);
  u += t;
  printf("%s %d\n", u.c_str(), (int)u.size());

  // Self-append onto a string that already carries an argc-dependent
  // suffix, then one more non-aliasing append on top of the aliased
  // result, so the staged value has to be the PRE-append operand.
  std::string w = "z";
  if (argc > 1)
    w += "9";
  w += w;
  w += t;
  w += w;
  printf("%s %d\n", w.c_str(), (int)w.size());
  return 0;
}
