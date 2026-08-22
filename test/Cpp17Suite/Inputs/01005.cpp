#include <iostream>
#include <string>

// W2.22: the first corpus entry that produces its output the way C++
// actually produces output. Every other entry in this suite has to declare
// `extern "C" int printf(const char *, ...);` because until this wave there
// was no other way to print at all.
//
// A cout chain mixing every ADMITTED operand type -- the integer widths and
// signednesses, bool, char / signed char / unsigned char (all three are
// CHARACTER overloads, and hi/sc/uc are >= 128 or negative so the raw byte
// is exercised), double and float through the %g funnel, std::string,
// string literals carrying Rust format metacharacters, std::endl, a bare
// `std::cout << std::endl;`, a chain with a SIDE-EFFECTING operand (whose
// own output must interleave in C++17 order), a trailing chain with no
// endl, and std::cerr.
//
// The ledger harness compares stdout only, so the cerr lines below are
// unchecked here on purpose; their byte oracle is
// test/EndToEnd/cpp-iostream.cpp, which diffs both streams.

static int trace = 0;

int bump(int k) {
  trace = trace * 10 + k;
  std::cout << "<b" << k << ">";
  return trace;
}

int scale(int v) { return v * 7 - 3; }

int main() {
  int i = scale(6);
  long l = (long)i * 1000000L;
  unsigned u = (unsigned)i + 4000000000u;
  long long ll = (long long)i * -1000000000000LL;
  unsigned long long ull = (unsigned long long)i * 1000000000000ULL;
  short sh = (short)(i - 100);
  unsigned short ush = (unsigned short)(60000 + i);
  bool on = i > 0;
  bool off = i < 0;
  char c = (char)('A' + (i % 26));
  char hi = (char)(200);
  signed char sc = (signed char)(-56);
  unsigned char uc = (unsigned char)(199);
  std::string s = "abc";
  s += "de";
  double third = (double)i / 3.0;
  double big = (double)i * 1e12;
  double tiny = (double)i * 1e-9;
  float f = (float)i / 4.0f;

  std::cout << "i=" << i << " l=" << l << " u=" << u << std::endl;
  std::cout << "ll=" << ll << " ull=" << ull << std::endl;
  std::cout << "sh=" << sh << " ush=" << ush << std::endl;
  std::cout << "on=" << on << " off=" << off << std::endl;
  std::cout << "c=" << c << " hi=" << hi << " sc=" << sc << " uc=" << uc
            << std::endl;
  std::cout << "s=" << s << std::endl;
  std::cout << "third=" << third << " big=" << big << " tiny=" << tiny
            << " f=" << f << std::endl;
  std::cout << "braces={} pct=%d tab:\there" << std::endl;
  std::cout << std::endl;
  std::cout << "seq:" << bump(1) << "," << bump(2) << "," << bump(3)
            << " trace=" << trace << std::endl;
  std::cout << "tail:" << i;
  std::cout << std::endl;
  std::cerr << "err i=" << i << std::endl;
  std::cout << "no-newline-tail:" << i;
  return 0;
}
