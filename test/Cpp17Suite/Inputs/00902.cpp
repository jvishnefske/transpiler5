// Cpp17Suite 00902: try/throw/catch of an int. UNSUPPORTED from the W2.0
// baseline until W2.24 flipped it (exceptions as Result threading through
// a synthesized closed data enum) -- the LAST frontier marker; the ledger
// stands complete at 35/35.
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
