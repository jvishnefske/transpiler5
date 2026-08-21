// Cpp17Suite 01001: function-template monomorphization -- one type
// parameter, two distinct instantiations (int and double) of each of two
// templates, with the second template's body calling the first (so
// `scale<double>` must resolve to `add<double>`, not `add<int>`).
extern "C" int printf(const char *, ...);

template <typename T>
T add(T a, T b) {
  return a + b;
}

template <typename T>
T scale(T a, int n) {
  T total = a;
  for (int i = 1; i < n; ++i)
    total = add(total, a);
  return total;
}

int main() {
  int i = add(2, 3);
  double d = add(1.25, 2.5);
  int si = scale(4, 3);
  double sd = scale(0.5, 3);
  printf("%d %.2f %d %.2f\n", i, d, si, sd);
  return 0;
}
