// Cpp17Suite 00603: task-006 target (R1 probe) -- structured bindings
// over std::array (the tuple-like protocol route).
extern "C" int printf(const char *, ...);

#include <array>

int main() {
  std::array<int, 3> a = {7, 21, 35};
  auto [x, y, z] = a;
  printf("%d %d %d\n", x, y, z);
  return 0;
}
