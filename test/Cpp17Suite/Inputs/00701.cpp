// Cpp17Suite 00701: task-007 target -- ranged-for by value over a
// DeclRefExpr vector range, no mutation of the range in the body (the
// conservative shape task-007's desugar supports first).
extern "C" int printf(const char *, ...);

#include <vector>

int main() {
  std::vector<int> v;
  v.push_back(2);
  v.push_back(5);
  v.push_back(11);
  int total = 0;
  for (int x : v) {
    total += x;
  }
  printf("%d\n", total);
  return 0;
}
