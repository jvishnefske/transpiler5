// FR-160b non-vacuity control: a C test binary that FAILS. meson and CTest
// both declare that a test passes iff it exits 0, so the wrapped `c_main`
// must go RED here. A generated suite that can only pass is exactly the
// vacuous pass FR-160 exists to forbid, and a compile-clean crate looks
// identical either way.
int printf(const char *, ...);

static int tally = 0;

int main(void) {
  for (int i = 1; i <= 4; i++)
    tally += i * i;
  printf("tally=%d\n", tally);
  return tally - 25;
}
