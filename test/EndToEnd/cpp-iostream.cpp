// REQUIRES: cargo
// W2.22: `std::cout` / `std::cerr` `<<` chains -> Rust `print!`/`eprint!`,
// end to end. THE oracle for the wave: the emitted crate's stdout AND
// stderr are diffed byte for byte against a `clang++ -std=c++17` build of
// the identical source. Nothing else can see these bugs -- every hazard
// below is a FORMATTING divergence or an ORDERING divergence, and a
// compile-clean `cargo build` is blind to both.
//
// This is a new EndToEnd shape: no existing leg diffs stderr against a
// native run. The two streams are captured to SEPARATE files and diffed
// separately, on purpose -- libstdc++ leaves cerr unit-buffered and cout
// block-buffered on a pipe, so a `2>&1` merge would diverge by construction
// and prove nothing.
//
// What each line is here to catch, all measured against clang++:
//  - double/float: libstdc++ `operator<<` is %g with 6 significant digits.
//    Rust's `{}` prints `0.3333333333333333` for 1/3 and `1000000` for 1e6;
//    `{:.6}` and `{:e}` are both wrong too (`{:e}` gives `1e6`, not `1e+06`).
//    Only the project's C-compatible %g helper reproduces it.
//  - bool: C++ prints 1/0, Rust's `{}` prints true/false.
//  - char, signed char AND unsigned char: all three are CHARACTER overloads
//    writing ONE raw byte -- including values >= 128, where the Display
//    funnel would emit two-byte UTF-8. `hi`/`sc`/`uc` below are all >= 128
//    (or negative) precisely to exercise that byte.
//  - SIDE-EFFECTING OPERANDS: C++17 writes E1's output BEFORE evaluating
//    E2, so `cout << "seq:" << bump(1) << ...` must interleave bump()'s own
//    output. Fusing the chain into one macro call reorders it -- measured
//    (`<b1><b2><b3>seq:...` instead of `seq:<b1>1,<b2>12,...`). This is the
//    single highest-risk line in the file.
//  - integer widths and signedness at both ends, brace/percent/tab bytes in
//    a literal (they are Rust format-string metacharacters), a bare
//    `std::cout << std::endl;`, and a trailing chain with NO endl (whose
//    un-flushed bytes must still reach the pipe across the process exit).
//
// Every value derives from argc, so no constant folding can pre-compute the
// answers and hide a miscompile behind a compile-clean crate. argv is a
// hard rejection here -- argc only.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out 2> %t.native.err
// RUN: %t.crate/target/release/cpp_iostream > %t.rust.out 2> %t.rust.err
// RUN: diff %t.native.out %t.rust.out
// RUN: diff %t.native.err %t.rust.err

#include <iostream>
#include <string>

extern "C" int printf(const char *, ...);

// A chain operand that WRITES to stdout itself: the ordering oracle.
static int trace = 0;
int bump(int k) {
  trace = trace * 10 + k;
  printf("<b%d>", k);
  return trace;
}

int main(int argc, char **) {
  int i = argc * 39;
  long l = (long)i * 1000000L;
  unsigned u = (unsigned)i + 4000000000u;
  long long ll = (long long)i * -1000000000000LL;
  unsigned long long ull = (unsigned long long)i * 1000000000000ULL;
  short sh = (short)(i - 100);
  unsigned short ush = (unsigned short)(60000 + i);
  bool on = i > 0;
  bool off = i < 0;
  char c = (char)('A' + (i % 26));
  char hi = (char)(200 + i - i);
  signed char sc = (signed char)(-56 + i - i);
  unsigned char uc = (unsigned char)(199 + i - i);
  std::string s = "abc";
  s += "de";
  double third = (double)i / 3.0;
  double big = (double)i * 1e12;
  double tiny = (double)i * 1e-9;
  double whole = (double)i;
  float f = (float)i / 4.0f;

  std::cout << "i=" << i << " l=" << l << " u=" << u << std::endl;
  std::cout << "ll=" << ll << " ull=" << ull << std::endl;
  std::cout << "sh=" << sh << " ush=" << ush << std::endl;
  std::cout << "on=" << on << " off=" << off << std::endl;
  std::cout << "c=" << c << " hi=" << hi << " sc=" << sc << " uc=" << uc
            << std::endl;
  std::cout << "s=" << s << std::endl;
  std::cout << "third=" << third << " big=" << big << " tiny=" << tiny
            << std::endl;
  std::cout << "whole=" << whole << " f=" << f << std::endl;
  std::cout << "braces={} pct=%d tab:\there" << std::endl;
  std::cout << std::endl;
  std::cout << "seq:" << bump(1) << "," << bump(2) << "," << bump(3)
            << " trace=" << trace << std::endl;
  std::cout << "tail:" << i;
  std::cout << std::endl;
  std::cout << "no-newline-tail:" << i;

  std::cerr << "err i=" << i << " third=" << third << std::endl;
  std::cerr << "err c=" << c << " hi=" << hi << std::endl;
  std::cerr << "err-tail:" << i << std::endl;
  return 0;
}
