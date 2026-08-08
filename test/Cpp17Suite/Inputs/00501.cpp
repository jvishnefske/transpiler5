// Cpp17Suite 00501: task-005 target -- std::pair<int, int> construction
// and .first/.second read + write.
extern "C" int printf(const char *, ...);

#include <utility>

int main() {
  std::pair<int, int> p(3, 40);
  p.first += 2;
  p.second = p.second + p.first;
  printf("%d %d\n", p.first, p.second);
  return 0;
}
