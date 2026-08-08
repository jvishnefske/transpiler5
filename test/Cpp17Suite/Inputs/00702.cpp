// Cpp17Suite 00702: task-007 stretch target (spike-gated) -- ranged-for
// with a reference loop variable mutating the elements in place. Enters
// the manifest only if the task-007 spike GOes on the mutation form.
extern "C" int printf(const char *, ...);

#include <vector>

int main() {
  std::vector<int> v;
  v.push_back(1);
  v.push_back(2);
  v.push_back(3);
  for (int &x : v) {
    x = x * 10;
  }
  printf("%d %d %d\n", v[0], v[1], v[2]);
  return 0;
}
