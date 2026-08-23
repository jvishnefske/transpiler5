// REQUIRES: cargo
// W2.24, the two legs the int-payload twin (cpp-exceptions.cpp) cannot
// carry: a DOUBLE payload (the carrier synthesis is per thrown payload
// type, so f64 needs its own TU — `ThrowsF64` here) and a W2.17 Drop
// object destroyed during unwinding. The Drop leg is an ORDERING oracle:
// C++ destroys the thrower's local AFTER evaluating the throw operand and
// BEFORE the handler runs; the image's early `return Err0` drops the
// callee's locals at the return, which is the same point — the spike
// measured Drop is NOT a discriminator, and this leg keeps that true on
// the real implementation. (A droppy local declared INSIDE the try block
// IS a divergence — function-scoped places would drop it late — and
// stays a located rejection, pinned in exceptions-invalid.cpp.)
//
// stdout AND stderr are diffed separately per the cpp-iostream.cpp
// precedent; both must be byte-identical, stderr empty on both sides.
// Every value derives from argc; argv is unused.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out 2> %t.native.err
// RUN: %t.crate/target/release/cpp_exceptions_drop > %t.rust.out 2> %t.rust.err
// RUN: diff %t.native.out %t.rust.out
// RUN: diff %t.native.err %t.rust.err

extern "C" int printf(const char *, ...);

class Tracer {
public:
  int tag;
  Tracer(int t) {
    tag = t;
    printf("ctor %d\n", tag);
  }
  ~Tracer() { printf("dtor %d\n", tag); }
};

double risky(double x) {
  Tracer t(1);
  if (x < 2.0)
    throw x + 0.5;
  return x * 2.0;
}

int main(int argc, char **) {
  double got = 0.0;
  // argc == 1 -> risky(1.0) throws 1.5; Tracer t must print "dtor 1"
  // BEFORE the handler assigns and the printf below runs.
  try {
    got = risky((double)argc);
    got = got + 100.0;
  } catch (double e) {
    got = e;
  }
  printf("caught %f\n", got);
  // The happy path: no throw, dtor still exactly once.
  try {
    got = risky((double)argc + 2.0);
  } catch (double e) {
    got = -1.0;
  }
  printf("clean %f\n", got);
  return 0;
}
