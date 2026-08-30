// REQUIRES: cargo
// FR-166 phase 2, the glibc `EPOLL_EVENTS` shape: a flags enum whose top
// enumerator is `1u << 31`.
//
// This is the reach argument for opening the 32-bit unsigned enumerator
// range. `EPOLLET = 1u << 31` makes clang pick a 32-bit `unsigned int`
// underlying type for the whole enum, so before FR-166 phase 2 the entire
// enum -- every one of its ordinary in-range flags with it -- was a located
// "enumerator value does not fit in i32" rejection, and every declaration
// mentioning `enum EPOLL_EVENTS` cascaded off it. `EPOLL_EVENTS` lives in
// glibc's <sys/epoll.h>, so the blast radius is much wider than the handful
// of systemd enums the FR-166 wave measured.
//
// The invariant pinned here is that the bit-31 flag survives every ordinary
// flags-enum operation with C's bits: OR-ing flags together, masking, the
// truth test on the masked result, and the round-trip back out through an
// `unsigned`. `1u << 31` is exactly the value whose i32 reinterpretation is
// negative, so a lost signedness anywhere on the path shows up as a sign
// extension in the `%lu` column rather than as a compile error -- `cargo
// build` cannot see it. The oracle is the stdout byte-diff against the
// clang-built native, with the flag set chosen by `argc`.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.n0.out && %t.crate/target/release/enum_flag_bit31 > %t.r0.out
// RUN: diff %t.n0.out %t.r0.out
// RUN: %t.native a > %t.n1.out && %t.crate/target/release/enum_flag_bit31 a > %t.r1.out
// RUN: diff %t.n1.out %t.r1.out
// RUN: %t.native a b > %t.n2.out && %t.crate/target/release/enum_flag_bit31 a b > %t.r2.out
// RUN: diff %t.n2.out %t.r2.out
// RUN: %t.native a b c > %t.n3.out && %t.crate/target/release/enum_flag_bit31 a b c > %t.r3.out
// RUN: diff %t.n3.out %t.r3.out

int printf(const char *, ...);

/* The shape of glibc's <sys/epoll.h> `enum EPOLL_EVENTS`, trimmed. */
enum FLAGS {
  F_IN = 0x001,
  F_OUT = 0x004,
  F_ERR = 0x008,
  F_ONESHOT = 1u << 30,
  F_ET = 1u << 31
};

int main(int argc, char **argv) {
  unsigned seeds[4] = {F_IN, F_ET, F_ET | F_IN, F_ONESHOT | F_ET | F_OUT};
  unsigned mask = seeds[(argc - 1) & 3];
  enum FLAGS f = (enum FLAGS)mask;

  printf("f: %u %lu\n", (unsigned)f, (unsigned long)f);
  printf("k: %u %lu %u\n", F_ET, (unsigned long)F_ET, F_ET | F_IN);
  printf("m: %d %d %d\n", (f & F_ET) != 0, (f & F_IN) != 0,
         (f & F_ONESHOT) != 0);
  printf("t: %d %d\n", (f & F_ET) ? 1 : 0, !(f & F_OUT));
  printf("c: %d %d %d\n", f == F_ET, f > F_ONESHOT, f >= F_IN);
  unsigned back = F_ET;
  printf("b: %u %lu\n", back, (unsigned long)(back | F_IN));
  return 0;
}
