// REQUIRES: cargo
// C99-43 front C1 differential: the check_cpu-shaped single-global-or-
// NULL out-param cursor (the spike's variant A, unguarded), byte-diffed
// against the clang-built native binary — THE oracle for the Option
// cell mapping (`T **efp` -> one `&mut Option<i64>`, None = NULL,
// Some(offset) = offset into the one statically-known global backing).
//
// BOTH branches are exercised: iterations 0..2 take the None path
// (`err == 0`, `if (ef)` false), iterations 3..5 take the Some path and
// read `ef[i]` through the offset routing into err_flags' backing —
// including a mid-loop mutation of the global between rounds, which a
// staged-copy miscompile (reading a stale backing) would surface as a
// byte diff. The callee also mutates its scalar-ref co-parameter so the
// Option cell's arity-preserving mapping (Q1) is pinned beside an
// ordinary in-out parameter.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name cursor_param_global --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cursor_param_global > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

unsigned err_flags[4] = {1u, 2u, 4u, 8u};

int check(int *lvl, unsigned **efp) {
  int err = *lvl > 2;
  *lvl = *lvl + 10;
  *efp = err ? err_flags : 0;
  return err;
}

int main(void) {
  int t = 0;
  while (t < 6) {
    int lvl = t;
    unsigned *ef;
    int r = check(&lvl, &ef);
    if (ef) {
      unsigned acc = 0;
      int i = 0;
      while (i < 4) {
        acc = acc + ef[i];
        i = i + 1;
      }
      printf("t %d r %d lvl %d flags %u %u %u %u acc %u\n", t, r, lvl,
             ef[0], ef[1], ef[2], ef[3], acc);
    } else {
      printf("t %d r %d lvl %d no flags\n", t, r, lvl);
    }
    err_flags[t % 4] = err_flags[t % 4] + 1u;
    t = t + 1;
  }
  return 0;
}
