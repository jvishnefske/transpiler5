// REQUIRES: cargo
// W2.28: a non-type-template-parameter monomorphization differential, end
// to end. One `addN<int N>` is instantiated at 7 and at -3 (the negative
// value is the shape that dies if the value suffix is not snake-safe:
// `add_n_-3` is a hard rustc parse failure, so the code must be `n3`);
// one `pick<bool Up>` at both truth values (bool is Integral-kind and
// must not fuse `pick<true>` with `pick<false>`); one `chr<char C>` at
// two chars; one MIXED `mulN<typename T, int N>` at `<int,3>` and
// `<long,3>`, which dies if the type code and the value code are
// concatenated in anything but template-parameter declaration order; and
// one `Grid<int R, int C>` whose ARRAY FIELD length and method both
// consume the parameters, at `<2,3>` and `<3,2>` (argument-order
// anti-fusion for records); and one SAME-SHAPED `Duo<int A, int B>` at
// `<1,23>` and `<12,3>`, the pair that silently fused (both camel to
// `Duo123`) when the value code was bare digits — the `v` prefix in
// `templateArgIntegralCode` exists because this line miscompiled without
// it. Every value derives from argc, so constant
// folding cannot pre-compute the answers and hide a miscompile behind a
// compile-clean crate. Byte-identical vs `clang++ -std=c++17`.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_template_nttp > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

template <int N>
int addN(int x) {
  return x + N;
}

template <bool Up>
int pick(int a, int b) {
  if (Up)
    return a;
  return b;
}

template <char C>
int chr(int x) {
  return x + C;
}

template <typename T, int N>
T mulN(T x) {
  return x * N;
}

template <int A, int B>
struct Duo {
  int pad;
  int mix() { return A * 100 + B; }
};

template <int R, int C>
struct Grid {
  int cells[R * C];
  int fill(int base) {
    int sum = 0;
    for (int i = 0; i < R * C; ++i) {
      cells[i] = base + i;
      sum += cells[i];
    }
    return sum;
  }
};

int main(int argc, char **argv) {
  int seed = argc; /* 1 at run time, opaque to the folder */
  Grid<2, 3> g23;
  Grid<3, 2> g32;
  Duo<1, 23> d123;
  Duo<12, 3> d1203;
  d123.pad = seed;
  d1203.pad = seed;
  int s23 = g23.fill(seed + 10);
  int s32 = g32.fill(seed + 20);
  printf("%d %d %d %d %d %d %ld %d %d %d %d %d\n", addN<7>(seed),
         addN<-3>(seed), pick<true>(seed + 1, seed + 2),
         pick<false>(seed + 1, seed + 2), chr<'A'>(seed),
         (int)mulN<int, 3>(seed + 4), mulN<long, 3>((long)seed + 5), s23, s32,
         g23.cells[5] + g32.cells[5], d123.mix() + d123.pad,
         d1203.mix() + d1203.pad);
  return 0;
}
