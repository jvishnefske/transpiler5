// Cpp17Suite 00904: frontier marker (corpus-only, no task) --
// std::optional<int> (C++17 vocabulary type): empty vs engaged,
// has_value(), value_or().
extern "C" int printf(const char *, ...);

#include <optional>

std::optional<int> find_even(int v) {
  if (v % 2 == 0)
    return v;
  return std::nullopt;
}

int main() {
  std::optional<int> a = find_even(8);
  std::optional<int> b = find_even(7);
  printf("%d %d %d %d\n", (int)a.has_value(), a.value_or(-1),
         (int)b.has_value(), b.value_or(-1));
  return 0;
}
