// REQUIRES: cargo
// FR-76 fn-ptr components through the parameter mapper, differential
// end-to-end test: a CALLBACK TABLE over byte buffers — struct fields of
// type `void (*)(uint8_t *, unsigned)` / `int (*)(const uint8_t *,
// unsigned)` import as fn_ptrs over region-typed slice refs, the bound
// functions (including the deref-only `poke`, which only the FR-76
// address-taken forcing makes slice-typed) keep signatures exactly equal
// to the field types, and every invocation through a field passes a
// `slice_of` region view. Transpile to a cargo crate, build it, and
// byte-diff its stdout against the natively compiled C program in BOTH
// argc branches: the seed and the table wiring derive from argc, so
// constant folding cannot hide a miscompile, and both mix callbacks and
// both fold callbacks execute over the same buffer in each run. main
// returns 0 and reports everything via printed lines, so lit's exit-code
// checking covers both runs and diff covers the observable behavior. The
// program has no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native0.out
// RUN: %t.crate/target/release/fnptr_slice_components > %t.rust0.out
// RUN: diff %t.native0.out %t.rust0.out
// RUN: %t.native seeded > %t.native1.out
// RUN: %t.crate/target/release/fnptr_slice_components seeded > %t.rust1.out
// RUN: diff %t.native1.out %t.rust1.out

#include <stdio.h>
#include <stdint.h>

struct Ops {
  void (*mix)(uint8_t *buf, unsigned len);
  int (*fold)(const uint8_t *buf, unsigned len);
};

void wave(uint8_t *buf, unsigned len) {
  for (unsigned i = 0; i < len; i++)
    buf[i] = (uint8_t)(buf[i] + 3u * i + 1u);
}

void poke(uint8_t *buf, unsigned len) {
  *buf = (uint8_t)(*buf + len);
}

int fold_sum(const uint8_t *buf, unsigned len) {
  int s = 0;
  for (unsigned i = 0; i < len; i++)
    s += buf[i];
  return s;
}

int fold_xor(const uint8_t *buf, unsigned len) {
  int x = 5;
  for (unsigned i = 0; i < len; i++)
    x ^= buf[i];
  return x;
}

int main(int argc, char **argv) {
  unsigned seed = (unsigned)argc * 37u + 11u;
  struct Ops table[2] = {{wave, fold_sum}, {poke, fold_xor}};
  if (argc > 1) {
    table[0].mix = poke;
    table[1].mix = wave;
  }
  uint8_t buf[16];
  for (unsigned i = 0; i < 16u; i++)
    buf[i] = (uint8_t)(seed + 7u * i);
  wave(buf, 16u);
  for (int k = 0; k < 2; k++) {
    table[k].mix(buf, 16u);
    int r = table[k].fold(buf, 16u);
    printf("k=%d r=%d b0=%u b15=%u\n", k, r, (unsigned)buf[0],
           (unsigned)buf[15]);
  }
  printf("seed=%u\n", seed);
  return 0;
}
