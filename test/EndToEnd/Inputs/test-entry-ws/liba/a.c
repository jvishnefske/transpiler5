// FR-160 slice 1 EndToEnd fixture -- the 'liba' workspace member.
//
// `a_sum` is the shared subject: the binary member calls it with an
// argc-derived seed (so no constant fold can hide a miscompile) and the
// generated `#[test]` calls it with a fixed one.
int a_sum(int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += i * 3 - 1;
  return s;
}

// The integer-shaped entry point: the C suite's own assertion, expressed the
// way meson and CTest already read one -- exit 0 means pass.
int test_alpha(void) { return a_sum(7) == 56 ? 0 : 1; }
