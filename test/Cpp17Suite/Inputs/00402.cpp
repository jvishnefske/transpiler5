// Cpp17Suite 00402: task-004 target -- std::array<double, N> subscript
// arithmetic; %.2f keeps the printed form deterministic across the native
// and transpiled legs.
extern "C" int printf(const char *, ...);

#include <array>

int main() {
  std::array<double, 3> a = {1.5, 2.25, 4.0};
  a[1] = a[1] * 2.0;
  double total = a[0] + a[1] + a[2];
  printf("%.2f %.2f\n", a[1], total);
  return 0;
}
