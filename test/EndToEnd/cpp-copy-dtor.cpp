// REQUIRES: cargo
// W2.23: copy constructor x destructor INTERLEAVING, end to end. The
// counting table (cpp-copy-ctor.cpp) proves copy COUNTS; this leg proves
// copy/dtor ORDER -- a class that printf()s in its ctor, copy ctor, AND
// dtor makes the full construction/destruction interleaving observable, so
// the byte-diff sees reverse-declaration drop order AROUND return copies
// and memberwise assignment, not just the copy tally.
//
// Deliberately WITHOUT by-value calls: passing a copy+dtor class by value
// is the one measured caller/callee drop-point divergence (native destroys
// the parameter temp at the end of the caller's full-expression; a
// move-into-the-callee image drops it inside the callee -- the spike's
// twocall probe), so it stays signature-level rejected
// (test/Import/Cpp/copy-ctor-invalid.cpp, DROPPYBYVAL). Return-by-value IS
// admitted: the return temp is moved out and never drops in the callee,
// while the named locals drop in reverse declaration order exactly as C++
// destroys them after the return copy -- correct by construction, verified
// byte-identical here.
//
// Every value derives from argc, so no constant folding can pre-compute the
// answers and hide a miscompile behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_copy_dtor > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct Loud {
  int id;
  Loud(int i) : id(i) { printf("ctor %d\n", id); }
  Loud(const Loud &o) : id(o.id + 100) { printf("copy %d->%d\n", o.id, id); }
  ~Loud() { printf("dtor %d\n", id); }
};

Loud two_ret(int pick, int x) {
  Loud p(x);
  Loud q(x + 1);
  if (pick)
    return p;
  return q;
}

int main(int argc, char **argv) {
  Loud a(argc);
  Loud b = a;
  // An if-branch copy (the W2.17 scope subset admits branch-body locals;
  // a BARE nested block stays scope-rejected, see destructors-invalid.cpp):
  // c is copy-constructed and dropped inside the branch, between b's
  // construction and t's.
  if (argc > 0) {
    Loud c = b;
    printf("inner %d\n", c.id);
  }
  Loud t = two_ret(argc, argc + 10);
  printf("got %d\n", t.id);
  Loud m(argc + 50);
  m = a;
  printf("assigned %d\n", m.id);
  return 0;
}
