// Cpp17Suite 00906: frontier marker (corpus-only, no task) --
// std::variant<int, double> (C++17 vocabulary type): index(),
// reassignment to the other alternative, std::get.
extern "C" int printf(const char *, ...);

#include <variant>

int main() {
  std::variant<int, double> v = 7;
  int idx_int = v.index();
  int as_int = std::get<int>(v);
  v = 2.5;
  int idx_dbl = v.index();
  double as_dbl = std::get<double>(v);
  printf("%d %d %d %.1f\n", idx_int, as_int, idx_dbl, as_dbl);
  return 0;
}
