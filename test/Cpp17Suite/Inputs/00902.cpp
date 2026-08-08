// Cpp17Suite 00902: frontier marker (corpus-only, no task) -- try/throw/
// catch of an int. Pins the W2.0 baseline "unsupported statement:
// CXXTryStmt" frontier as an UNSUPPORTED corpus entry.
extern "C" int printf(const char *, ...);

int checked_div(int num, int den) {
  if (den == 0)
    throw 42;
  return num / den;
}

int main() {
  int caught = 0;
  int ok = 0;
  try {
    ok = checked_div(12, 3);
    checked_div(1, 0);
  } catch (int e) {
    caught = e;
  }
  printf("%d %d\n", ok, caught);
  return 0;
}
