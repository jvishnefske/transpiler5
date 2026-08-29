// REQUIRES: cargo
// FR-106: a crate whose own `[lints.rust] unused_assignments = "deny"`
// table rejects it is an emitter defect, not a user problem.
//
// This is heatshrink's `find_longest_match` reduced (heatshrink_encoder.c:
// the `len` scan). `len` is reset to 0, then a candidate that mismatches at
// `match_maxlen` does `pos = index[pos]; continue;` -- so on THAT path the
// `len = 0` store has no reader on any continuation: the next iteration
// overwrites `len` before reading it, and the post-loop tail reads only
// `match_maxlen`/`match_index`. rustc's `unused_assignments` says exactly
// that ("value assigned to `len` is never read"), and with the FR-53 deny
// the emitted crate FAILED TO BUILD with no emitter diagnostic anywhere:
// exit 0, unbuildable output. Deleting the store instead is the fenced
// cross-iteration dead-store elision (CLAUDE.md: miscompiled three times),
// so the fix ADDS `#[allow(unused_assignments)]` to the one fn that holds
// such a store and deletes nothing.
//
// The predicate is ALL-PATHS: the store must have no reader on ANY
// continuation, including the loop's back edge and every `break` exit.
// test/Driver/unused-assignment-allow-negative.c pins the shapes a
// some-path predicate would mark and this one must not.
//
// `cargo build` success is not the oracle here -- it only proves the deny
// is satisfied. The stdout diff against the clang-built native is: the
// attribute must not perturb a single emitted byte of behavior, and the
// seeds derive from argc so constant folding cannot pre-compute the scan
// and hide a miscompile. Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/unused_assignment_allow > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a b > %t.native3.out
// RUN: %t.crate/target/release/unused_assignment_allow a b > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out
//
// The attribute lands on the two functions that hold such a store, and on
// no other -- a crate-wide re-allow would delete the tripwire that found
// this. `tu0_f__ind` additionally STACKS with FR-140's
// `#[allow(non_snake_case)]` (its locals carry interior double
// underscores): both attributes must be present, on the same item, in a
// fixed order, and the crate must build with BOTH lints denied.
// RUN: FileCheck %s < %t.crate/src/main.rs
// CHECK-NOT:  allow(unused_assignments)
// CHECK:      #[allow(unused_assignments)]
// CHECK-NEXT: fn tu0_find_longest_match
// CHECK-NOT:  allow(unused_assignments)
// CHECK:      #[allow(non_snake_case)]
// CHECK-NEXT: #[allow(unused_assignments)]
// CHECK-NEXT: fn tu0_f__ind
// CHECK-NOT:  allow(unused_assignments)

#include <stdio.h>
#include <stdint.h>

static uint8_t buf[64];
static int16_t index_[32];

static uint16_t find_longest_match(uint16_t start, uint16_t end,
                                   uint16_t maxlen) {
  uint16_t match_maxlen = 0;
  uint16_t match_index = 0xFFFF;
  uint16_t len = 0;
  uint8_t *const needlepoint = &buf[end];
  int16_t pos = index_[end];
  while (pos - (int16_t)start >= 0) {
    uint8_t *const pospoint = &buf[pos];
    /* Dead on the `continue` path below, live on every other. */
    len = 0;
    if (pospoint[match_maxlen] != needlepoint[match_maxlen]) {
      pos = index_[pos];
      continue;
    }
    for (len = 1; len < maxlen; len++) {
      if (pospoint[len] != needlepoint[len]) break;
    }
    if (len > match_maxlen) {
      match_maxlen = len;
      match_index = pos;
      if (len == maxlen) break;
    }
    pos = index_[pos];
  }
  printf("maxlen=%u index=%u\n", match_maxlen, match_index);
  return match_index;
}

/* The same shape, with names that also trip `non_snake_case`: the FR-140
   allow and the FR-106 allow must STACK on one item and the crate must
   still build with both lints denied. */
static unsigned short f__ind(unsigned short start, unsigned short end,
                             unsigned short maxlen) {
  unsigned short m__ax = 0, m__idx = 0xFFFF, l__en = 0;
  uint8_t *const n__p = &buf[end];
  int16_t pos = index_[end];
  while (pos - (int16_t)start >= 0) {
    uint8_t *const p__p = &buf[pos];
    l__en = 0;
    if (p__p[m__ax] != n__p[m__ax]) {
      pos = index_[pos];
      continue;
    }
    for (l__en = 1; l__en < maxlen; l__en++)
      if (p__p[l__en] != n__p[l__en]) break;
    if (l__en > m__ax) {
      m__ax = l__en;
      m__idx = pos;
      if (l__en == maxlen) break;
    }
    pos = index_[pos];
  }
  printf("m__ax=%u\n", m__ax);
  return m__idx;
}

int main(int argc, char **argv) {
  int i;
  /* 64 wide so `pospoint[len]` (pos < 32, len <= 8) stays in bounds. */
  for (i = 0; i < 64; i++) buf[i] = (uint8_t)(argc * 7u + i % 5u);
  for (i = 0; i < 32; i++) index_[i] = (int16_t)(i - argc * 3);
  printf("r=%u\n", find_longest_match(0, (uint16_t)(16 + argc), 8));
  printf("r=%u\n", find_longest_match(1, (uint16_t)(20 - argc), 4));
  printf("s=%u\n", f__ind(0, (uint16_t)(16 + argc), 8));
  return 0;
}
