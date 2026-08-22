// REQUIRES: cargo
// FR-116, manifestation 1, end to end: a non-const global TOUCHED FROM A
// C++ METHOD BODY. Before the fix every shape below aborted the compile
// with `'emitrust.global_load' op 'G' does not reference a valid
// emitrust.global` -- the actor plan cannot see method bodies (they are not
// FR-40 item-graph nodes) and neither can the pass's SymbolTable use
// queries (an `emitrust.impl` is a nested symbol table), so the global was
// localized into c_main and then erased under a live reference. THIS TEST
// IS THE ORACLE for the fix: `cargo build` succeeding proves nothing here,
// because the demotion path rewrites where the global LIVES, so the crate's
// stdout is diffed byte for byte against a `clang++ -std=c++17` build of
// the identical source.
//
// Covered, all of them separately measured as verifier aborts before the
// fix: a READ from an ordinary method, a WRITE from a method (so the
// thread-local Cell must round-trip a store, not just a load), a read from
// a CONSTRUCTOR member-initializer list, a write from a W2.17 DESTRUCTOR
// body (an access that happens at a point no call site names), a read from
// a method of a W2.16 CLASS-TEMPLATE instantiation at two instantiations,
// and a global touched from BOTH a method and c_main (the mixed shape,
// where the driver-local plan was not even wrong about c_main).
//
// Every value derives from argc, so no constant folding can pre-compute the
// answers and hide a miscompile behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_method_global > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

int g_read = 7;
int g_write = 0;
int g_ctor = 3;
int g_dtor = 0;
int g_tmpl = 11;
int g_both = 5;

struct Peek {
  int v;
  Peek(int x) : v(x) {}
  int peek() { return g_read + v; }
  void stir(int d) { g_write = g_write + d + v; }
};

struct CtorRead {
  int v;
  CtorRead(int x) : v(x + g_ctor) {}
  int get() { return v; }
};

struct DtorWrite {
  int v;
  DtorWrite(int x) : v(x) {}
  ~DtorWrite() { g_dtor = g_dtor + v; printf("dtor %d -> %d\n", v, g_dtor); }
};

static void run_dtor(int n) {
  DtorWrite a(n);
  DtorWrite b(n + 1);
}

template <typename T>
struct W {
  T v;
  W(T x) : v(x) {}
  T peek() { return v + (T)g_tmpl; }
};

struct Both {
  int v;
  Both(int x) : v(x) {}
  int peek() { return g_both + v; }
};

int main(int argc, char **) {
  Peek p(argc);
  int a = p.peek();
  p.stir(argc);
  p.stir(argc + 1);
  CtorRead c(argc);
  run_dtor(argc + 1);
  W<int> wi(argc * 2);
  W<long> wl(argc + 4);
  Both b(argc);
  g_both = g_both + argc;
  printf("a=%d write=%d c=%d dtor=%d wi=%d wl=%d b=%d both=%d\n", a, g_write,
         c.get(), g_dtor, wi.peek(), (int)wl.peek(), b.peek(), g_both);
  return 0;
}
