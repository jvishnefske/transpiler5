// Cpp17Suite 00301: task-003 target -- vector front()/back()/pop_back().
// front/back on an empty vector is C++ UB, so the corpus only calls them
// on non-empty vectors; a Rust panic-on-empty spelling is then a safe
// refinement that byte-matches on every defined input.
extern "C" int printf(const char *, ...);

#include <vector>

int main() {
  std::vector<int> v;
  v.push_back(4);
  v.push_back(8);
  v.push_back(15);
  int f = v.front();
  int b = v.back();
  v.pop_back();
  int b2 = v.back();
  int n = v.size();
  printf("%d %d %d %d\n", f, b, b2, n);
  return 0;
}
