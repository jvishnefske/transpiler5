// Cpp17Suite 00202: task-002 target -- C++17 switch-with-initializer
// (`switch (init; cond)`), the init variable read inside case bodies so
// its scope must span the whole switch.
extern "C" int printf(const char *, ...);

int label(int v) {
  switch (int r = v % 3; r) {
  case 0:
    return 100 + r;
  case 1:
    return 200 + r + v;
  default:
    return 300 + r;
  }
}

int main() {
  printf("%d %d %d\n", label(6), label(7), label(8));
  return 0;
}
