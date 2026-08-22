// REQUIRES: cargo
// FR-117 byte-diff oracle for the NEWLY ADMITTED shape: a class carrying TWO
// user-defined conversion functions. On unpatched HEAD this exact source did
// not import at all -- both conversion functions mangled to the same EMPTY
// base name and the second one was rejected with
// `error: unsupported: conflicting definition of 'C_'` -- so `--emit=crate`
// produced nothing. Omitting a conversion function (FR-117) admits the class,
// and this is the oracle that the REST of the class, which is what the
// program actually runs, still behaves exactly like C++.
//
// STRICT mode on purpose (no --recover): the point of the pin is that the
// translation unit has ZERO rejections now, not that recovery papers over
// one.
//
// The conversion functions are declared but never used, because every use of
// one is still a located rejection (see
// test/Import/Cpp/cpp-conversion-function-invalid.cpp). What must be
// byte-identical is the ordinary member surface of a class that carries them
// -- constructor, const method, mutating method, overload set -- since the
// overload-count loop in `cxxMethodMangledName` walks every method of the
// class, omitted ones included.
//
// Every value derives from argc, so no constant folding can pre-compute the
// answers and hide a miscompile behind a compile-clean crate; `argv` is
// declared (main's standard two-parameter form) but never touched, since
// reading it is a hard rejection.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_conversion_function > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct Cell {
  int v;

  Cell(int start) : v(start) { printf("ctor %d\n", v); }

  int get() const { return v; }
  int get(int bias) const { return v + bias; }
  void bump(int d) {
    v += d;
    printf("bump -> %d\n", v);
  }

  // Both omitted from the import; neither is called.
  operator int() const { return v; }
  operator long() const { return v + 1; }
};

// A second class, whose only members are conversion functions: after the
// omission it has no imported methods at all and must still emit a usable
// struct.
struct Pair {
  int lo;
  int hi;
  operator int() const { return lo + hi; }
};

static int total(int n) {
  Pair p;
  p.lo = n;
  p.hi = n * 3;
  return p.lo + p.hi;
}

int main(int argc, char **argv) {
  Cell c(argc + 4);
  c.bump(argc);
  int a = c.get();
  int b = c.get(argc * 2);
  int t = total(argc);
  printf("a=%d b=%d t=%d\n", a, b, t);
  return 0;
}
