// REQUIRES: cargo
// W2.28: an explicit-specialization differential, end to end. One
// `code<T>` has a hand-written full specialization at `int` and implicit
// instantiations at `char` and `long` — the call at `int` must dispatch
// to the HAND-WRITTEN body under the same suffixed symbol the implicit
// instantiation would have used, and the neighbours must keep the
// primary's body. One `Tag<T>` has a full class specialization at `int`
// whose FIELD SET differs from the primary's (the shape that dies if the
// specialization is admitted carrying the primary's pattern), with a
// constructor and a method on both bodies. Every value derives from
// argc, so constant folding cannot pre-compute the answers.
// Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_template_explicit_spec > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

template <typename T>
int code(T v) {
  return 1 + (int)v;
}

template <>
int code<int>(int v) {
  return 100 + v;
}

template <typename T>
struct Tag {
  T v;
  Tag(T x) : v(x) {}
  int id() { return (int)v + 1; }
};

template <>
struct Tag<int> {
  int v;
  int bonus;
  Tag(int x) : v(x), bonus(40) {}
  int id() { return v + bonus; }
};

int main(int argc, char **argv) {
  int seed = argc; /* 1 at run time, opaque to the folder */
  Tag<int> ti(seed + 2);
  Tag<char> tc((char)(seed + 3));
  Tag<long> tl((long)seed + 4);
  printf("%d %d %d %d %d %d\n", code(seed), code((char)(seed + 60)),
         code((long)seed + 7), ti.id(), tc.id(), tl.id());
  return 0;
}
