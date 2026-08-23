// REQUIRES: cargo
// W2.28: a partial-specialization differential, end to end. One `W<T>`
// with a POINTER partial `W<T*>` is instantiated at `int`, `int*` and
// `double*` — the pointer instantiations must carry the PARTIAL's body
// and the plain one the primary's, under the primary-argument suffixes
// (`w_pi32` vs `w_i32`) that keep them distinct types. One `P<A, B>`
// with a SECOND-ARGUMENT partial `P<A, int>` pins that selection is not
// first-arg-only, and its field type comes from the pattern
// substitution. One MIXED `Arr<T, int N>` with partial `Arr<T*, N>`
// pins a type argument and a W2.28 value argument composing in one
// suffix on a partial's instantiation. Every value derives from argc,
// so constant folding cannot pre-compute the answers.
// Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_template_partial_spec > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

template <typename T>
struct W {
  int k(int x) { return x + 1; }
};

template <typename T>
struct W<T *> {
  int k(int x) { return x + 2; }
};

template <typename A, typename B>
struct P {
  A v;
  int m() { return (int)v + 10; }
};

template <typename A>
struct P<A, int> {
  A v;
  int m() { return (int)v + 20; }
};

template <typename T, int N>
struct Arr {
  T data[N];
  int tag() { return 1000 + N; }
};

template <typename T, int N>
struct Arr<T *, N> {
  int hits;
  int tag() { return 2000 + N + hits; }
};

int main(int argc, char **argv) {
  int seed = argc; /* 1 at run time, opaque to the folder */
  W<int> w1;
  W<int *> w2;
  W<double *> w3;
  P<char, char> p1;
  P<long, int> p2;
  p1.v = (char)(seed + 3);
  p2.v = (long)seed + 4;
  Arr<int, 4> a4;
  Arr<int *, 5> a5;
  a4.data[0] = seed + 6;
  a4.data[3] = seed + 7;
  a5.hits = seed + 8;
  printf("%d %d %d %d %d %d %d %d\n", w1.k(seed), w2.k(seed), w3.k(seed),
         p1.m(), p2.m(), a4.tag() + a4.data[0] + a4.data[3], a5.tag(),
         (int)sizeof(a4.data) / (int)sizeof(a4.data[0]));
  return 0;
}
