// Cpp17Suite 00703: task-007 target -- ranged-for by value over
// std::array (exercises the task-004-generalized index-place helper from
// the loop desugar).
extern "C" int printf(const char *, ...);

#include <array>

int main() {
  std::array<int, 4> a = {3, 6, 9, 12};
  int total = 0;
  for (int x : a) {
    total += x * 2;
  }
  printf("%d\n", total);
  return 0;
}
