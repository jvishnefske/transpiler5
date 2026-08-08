// Cpp17Suite 00201: task-002 target -- C++17 if-with-initializer
// (`if (init; cond)`), the else branch also reading the init variable, so
// a desugar that scopes the variable to the then-branch only would
// miscompile, not merely reject.
extern "C" int printf(const char *, ...);

int classify(int v) {
  if (int doubled = v * 2; doubled > 10) {
    return doubled;
  } else {
    return -doubled;
  }
}

int main() {
  printf("%d %d\n", classify(7), classify(3));
  return 0;
}
