// FR-160 slice 1 EndToEnd fixture -- the 'libb' workspace member.
int b_mix(int n) {
  int acc = 1;
  for (int i = 1; i <= n; ++i)
    acc = acc * 2 + i;
  return acc;
}

// The void-shaped entry point, run for its panics.
void test_beta(void) {
  int v = b_mix(4);
  (void)v;
}

// The NON-VACUITY control: a C entry point that would have exited 7. Its
// generated test must go RED. A `cargo test` that cannot fail proves nothing.
int test_fails(void) { return 7; }
