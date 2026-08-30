// REQUIRES: cargo
// FR-170: the CONTROLS for the i64::MAX switch-label fence.
//
// The fence added for FR-170 rejects exactly one case label, `i64::MAX`,
// because the upstream `scf.index_switch` verifier stores its labels in a
// `DenseSet<int64_t>` whose EMPTY key is that value. The danger of any such
// fence is that it is drawn too wide: `DenseMapInfo` reserves a TOMBSTONE
// key as well, and a fence written against "the reserved keys" rather than
// against the measured failure would also swallow labels that compile
// perfectly well today.
//
// This test pins the two immediate neighbours as still-supported:
//   * `i64::MIN`  (-9223372036854775808), the tombstone for a signed
//     integral that is not `long`; and
//   * `i64::MAX - 1` (9223372036854775806), which IS the `int64_t` tombstone
//     on LP64, where `int64_t` is `long`.
//
// "The compile succeeded" is NOT the oracle here -- a label silently routed
// to the wrong arm is compile-clean. The oracle is a byte-diff of stdout
// against the clang-built native, with the scrutinee chosen by `argc` so no
// constant folding on either side can evaluate the switch at build time.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/switch_case_int64_boundary > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/switch_case_int64_boundary a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/switch_case_int64_boundary a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/switch_case_int64_boundary a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out
// RUN: %t.native a b c d > %t.n4.out && %t.crate/target/release/switch_case_int64_boundary a b c d > %t.r4.out
// RUN: diff %t.n4.out %t.r4.out

int printf(const char *, ...);

int on_signed(long long v) {
  switch (v) {
  case (-9223372036854775807LL - 1): /* i64::MIN */
    return 1;
  case 9223372036854775806LL: /* i64::MAX - 1, the LP64 tombstone key */
    return 2;
  case 0:
    return 3;
  case -1:
    return 4;
  default:
    return 0;
  }
}

/* The same two bit patterns reached through an UNSIGNED scrutinee, where the
   labels zero-extend rather than sign-extend. 0x8000000000000000 is i64::MIN
   read as unsigned; 0x7FFFFFFFFFFFFFFE is i64::MAX-1. */
int on_unsigned(unsigned long long v) {
  switch (v) {
  case 9223372036854775808ull:
    return 11;
  case 9223372036854775806ull:
    return 12;
  default:
    return 10;
  }
}

int main(int argc, char **argv) {
  long long seeds[5] = {-9223372036854775807LL - 1, 9223372036854775806LL, 0,
                        -1, 77};
  long long s = seeds[(argc - 1) % 5];
  unsigned long long u = (unsigned long long)s;

  printf("s: %lld -> %d\n", s, on_signed(s));
  printf("u: %llu -> %d\n", u, on_unsigned(u));
  printf("k: %d %d %d %d\n", on_signed(-9223372036854775807LL - 1),
         on_signed(9223372036854775806LL), on_unsigned(9223372036854775808ull),
         on_unsigned(9223372036854775806ull));
  return 0;
}
