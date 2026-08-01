// REQUIRES: cargo
// Stage-A regression: a class that pairs an explicitly defaulted default
// constructor (`C() = default;`) with a real user constructor used to SIGSEGV
// the importer. Here the class imports, builds, and runs end to end, and its
// stdout byte-matches the clang++-native build. The program deliberately does
// NOT default-construct `C`: a defaulted default ctor leaves `int x`
// indeterminate (no NSDMI), so default-constructing would be UB and the diff
// meaningless; the `C() = default;` member is present only to exercise the
// fixed import path (it is iterated and skipped), while all live state flows
// through the well-defined `C(int)` constructor. main returns 0 and reports
// via printf, so lit's per-command exit-code check covers both runs and diff
// covers observable behavior.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_defaulted_ctor > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct C {
  int x;
  C(int v) { x = v; }
  C() = default;
};

int main() {
  C a(7);
  a.x += 5;
  C b(-3);
  printf("%d %d %d\n", a.x, b.x, a.x + b.x);
  return 0;
}
