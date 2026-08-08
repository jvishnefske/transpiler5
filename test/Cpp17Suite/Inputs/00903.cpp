// Cpp17Suite 00903: frontier marker (corpus-only, no task) -- lambda with
// a by-value capture, called twice.
extern "C" int printf(const char *, ...);

int main() {
  int base = 30;
  auto add_base = [base](int x) { return base + x; };
  printf("%d %d\n", add_base(4), add_base(12));
  return 0;
}
