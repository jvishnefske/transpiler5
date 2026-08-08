// Cpp17Suite 00101: day-one PASS -- std::vector<int> usage inside the W2.3
// subset (default ctor, push_back, size, operator[], at, empty, clear).
// Seeds the expected-pass manifest; a regression here means the vector
// recognition path broke, not that the frontier moved. Two distinct seeds
// so a wiring bug cannot hide behind a repeated literal.
extern "C" int printf(const char *, ...);

#include <vector>

int sum_vector(int seed) {
  std::vector<int> v;
  v.push_back(seed);
  v.push_back(seed * 3);
  v.push_back(seed + 11);
  int n = v.size();
  int a = v[0];
  int b = v.at(1);
  int c = v[2];
  int eb = v.empty();
  v.clear();
  int ea = v.empty();
  return n * 1000 + a + b + c + eb * 100 + ea * 10;
}

int main() {
  printf("%d %d\n", sum_vector(5), sum_vector(9));
  return 0;
}
