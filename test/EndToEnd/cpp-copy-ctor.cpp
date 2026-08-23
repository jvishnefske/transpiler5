// REQUIRES: cargo
// W2.23: user copy constructors, end to end. THE oracle for the wave: the
// emitted crate's stdout is diffed byte for byte against a
// `clang++ -std=c++17` build of the identical source. A copy constructor
// that COUNTS makes every copy directly observable, so this differential is
// the only thing that can see a materialized copy C++17 elides (or an
// elided copy C++ mandates) behind a compile-clean crate.
//
// This is the spike's full 7-row elision table, every row fixed by the
// standard (verified byte-identical across clang++, g++, AND both under
// -fno-elide-constructors):
//   T b = a;                 1 copy   (place-init)
//   take(a) by value         1 copy   (temp place, moved into the callee)
//   take(T(x)) prvalue arg   0 copies (guaranteed elision = absent AST node)
//   T f = factory(x);        0 copies (prvalue factory return, 00801's row)
//   two-return-object fn     1 copy   (NO NRVO flag with two candidates)
//   returning a by-value     2 copies (call-site copy + return copy)
//     parameter
//   m = a;                   0 copies (implicit operator= runs, memberwise)
// The ONE implementation-defined row -- the NRVO-candidate return -- is
// pinned REJECTED in test/Import/Cpp/copy-ctor-invalid.cpp.
//
// The counter is a global mutated inside a method (FR-116's lift demotes it
// with a warning), and -- the standing lesson -- it is NEVER read in the
// same full-expression as a copy: argument evaluation order is unspecified
// and was measured divergent between g++ and clang++.
//
// Every value derives from argc, so no constant folding can pre-compute the
// answers and hide a miscompile behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_copy_ctor > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

int copies = 0;

struct Tracer {
  int v;
  Tracer(int x) : v(x) {}
  Tracer(const Tracer &o) : v(o.v) { copies += 1; }
};

int take(Tracer t) { return t.v; }

Tracer factory(int x) { return Tracer(x); }

Tracer two_ret(int pick, int x) {
  Tracer p(x);
  Tracer q(x + 1);
  if (pick)
    return p;
  return q;
}

Tracer through(Tracer t) { return t; }

int main(int argc, char **argv) {
  Tracer a(argc + 40);
  Tracer b = a;
  printf("init v=%d copies=%d\n", b.v, copies);
  copies = 0;
  int r1 = take(a);
  printf("byval r=%d copies=%d\n", r1, copies);
  copies = 0;
  int r2 = take(Tracer(argc + 40));
  printf("prvarg r=%d copies=%d\n", r2, copies);
  copies = 0;
  Tracer f = factory(argc + 40);
  printf("factory v=%d copies=%d\n", f.v, copies);
  copies = 0;
  Tracer t2 = two_ret(argc, argc + 40);
  printf("tworet v=%d copies=%d\n", t2.v, copies);
  copies = 0;
  Tracer th = through(a);
  printf("through v=%d copies=%d\n", th.v, copies);
  copies = 0;
  Tracer m(argc);
  m = a;
  printf("assign v=%d copies=%d\n", m.v, copies);
  return 0;
}
