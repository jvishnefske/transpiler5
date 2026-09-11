// REQUIRES: cargo
// FR-234 rung 3: `strtoX(argv[i], &end, base)` and `end == argv[i]`, the two
// argv use FORMS that 004_nineality_sieve / 005_static_loop actually use,
// byte-diffed against the clang-built native.
//
// WHAT THIS PINS AND WHY ONLY A DIFF CAN SEE IT. Rungs 1 and 2 proved the
// strto* value and the endptr cursor over a LOCAL char region. Rung 3 moves
// the subject string into `main`'s argv table, which is a run of DISJOINT
// objects selected at RUNTIME -- not one object with a static base. That is
// the whole difficulty and it is invisible to every other oracle in the tree:
//
//   1 `end == argv[i]` is C 7.22.1.4p7's no-conversion test, and after
//     `strtol(argv[1], &end, b)` with nothing to convert the stored pointer
//     is argv[1] ITSELF -- byte offset 0. A lowering that models an
//     argv-rooted pointer as a bare byte CURSOR (which is what every other
//     base-less region in this importer is) answers TRUE for `end == argv[2]`
//     as well, because that is offset 0 too. Both crates compile, both
//     `cargo build` clean, and the wrong one prints a different `cross`
//     column here. The `cross12`/`cross21` columns exist for exactly that
//     bug and nothing else reaches it.
//   2 One `char *end` is REBOUND across two different argv arguments (the
//     006_static_alias shape), so the argument index has to be runtime state
//     that the second call overwrites. Freezing it at the first binding
//     leaves the second `end == argv[2]` reading the first argument.
//   3 An argv-rooted pointer is base-less, which is also how a statically
//     NULL pointer looks in this importer. C guarantees the opposite: the
//     endptr is never null. `end != NULL` and `if (end)` are the columns
//     that catch the fold going the wrong way -- and neither mentions argv,
//     so neither is screened out by the argv admission grammar.
//   4 The whole family rides one channel, so `strtoul` and `strtod` take
//     argv arguments too. strtod's subject is chosen so no vector reaches
//     the FR-235 hexadecimal-float or NaN-payload panics (they have no
//     byte-diff image at all; `libc-atof-loud-stop.c` owns them).
//
// The base is read at an argc-derived index and every conversion reads a
// real argv argument, so NEITHER compiler is handed a constant: no folded
// literal can stand in for a working parse, a working cursor, or a working
// argument index. argv[0] is never printed (the crate and the native live at
// different paths, so echoing it would diverge spuriously in lit).
//
// The vectors cover: a missing argument (argc == 1), a single argument, two
// successful parses, trailing garbage, a NO-CONVERSION argument on each side
// independently, empty arguments, leading whitespace with a sign, and a
// float with an exponent. No UB anywhere; main always returns 0.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name argv_strtox_endptr --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/argv_strtox_endptr > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native 42 > %t.native.out
// RUN: %t.crate/target/release/argv_strtox_endptr 42 > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native 42 7 > %t.native.out
// RUN: %t.crate/target/release/argv_strtox_endptr 42 7 > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native 42abc xyz > %t.native.out
// RUN: %t.crate/target/release/argv_strtox_endptr 42abc xyz > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native zzz 99 > %t.native.out
// RUN: %t.crate/target/release/argv_strtox_endptr zzz 99 > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native "" "" > %t.native.out
// RUN: %t.crate/target/release/argv_strtox_endptr "" "" > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native "  -17ff" "3.5e2rest" > %t.native.out
// RUN: %t.crate/target/release/argv_strtox_endptr "  -17ff" "3.5e2rest" > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native -1 12345678901 extra > %t.native.out
// RUN: %t.crate/target/release/argv_strtox_endptr -1 12345678901 extra > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

#include <stdio.h>
#include <stdlib.h>

/* Read at an argc-derived index so the base is never a folded constant. */
static const int bases[4] = {10, 0, 16, 8};

int main(int argc, char **argv) {
  char *end;
  long a;
  unsigned long u;
  double d;
  int base = bases[(argc - 1) % 4];

  printf("argc=%d base=%d\n", argc, base);

  if (argc < 2) {
    printf("no arguments\n");
    return 0;
  }

  /* Form 1 + form 2 over argv[1]: the 004/005 shape verbatim. */
  a = strtol(argv[1], &end, base);
  printf("a=%ld a_noconv=%d a_conv=%d\n", a, end == argv[1], end != argv[1]);
  /* The endptr is never null, whatever the conversion did (7.22.1.4p7). */
  printf("a_nonnull=%d a_truth=%d\n", end != (char *)0, end ? 1 : 0);

  if (argc < 3) {
    printf("one argument only\n");
    return 0;
  }

  /* A cursor into argv[1] is NOT a cursor into argv[2], even at offset 0. */
  printf("cross12=%d\n", end == argv[2]);

  /* The same `end` rebinds to a different argv argument (the 006 shape). */
  a = strtol(argv[2], &end, base);
  printf("b=%ld b_noconv=%d\n", a, end == argv[2]);
  printf("cross21=%d\n", end == argv[1]);

  /* The rest of the hosted family over argv too. */
  u = strtoul(argv[1], &end, base);
  printf("u=%lu u_noconv=%d u_cross=%d\n", u, end == argv[1],
         end == argv[2]);

  d = strtod(argv[2], &end);
  printf("d=%.17g d_noconv=%d d_cross=%d\n", d, end == argv[2],
         end == argv[1]);

  /* A RUNTIME argv index. Every comparison above has literal indices, which
     both compilers fold (`argv[1] == argv[2]` is statically false), so none
     of them can see the SELECTOR being carried at runtime. Here `k` is
     argc-derived: the emitted crate has to remember which argument the
     endptr walked, and `r_other` is the column that catches it forgetting.
     Both indices are in range because argc >= 3 on this path. */
  {
    int k = 1 + ((argc - 3) % 2);
    int o = 3 - k;
    long r = strtol(argv[k], &end, base);
    printf("r=%ld k=%d r_self=%d r_other=%d\n", r, k, end == argv[k],
           end == argv[o]);
  }

  return 0;
}
