// Cpp17Suite 00401: task-004 target -- std::array<int, N>: aggregate
// initialization, subscript read/write, size().
extern "C" int printf(const char *, ...);

#include <array>

int main() {
  std::array<int, 4> a = {10, 20, 30, 40};
  a[2] = a[0] + a[1] + 5;
  int n = a.size();
  printf("%d %d %d %d %d\n", a[0], a[1], a[2], a[3], n);
  return 0;
}
