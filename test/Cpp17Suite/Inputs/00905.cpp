// Cpp17Suite 00905: frontier marker (corpus-only, no task) --
// std::string_view (C++17 vocabulary type) over a literal: size(),
// remove_prefix(), indexed char.
extern "C" int printf(const char *, ...);

#include <string_view>

int main() {
  std::string_view sv = "cpp17-corpus";
  int full = sv.size();
  sv.remove_prefix(6);
  printf("%d %d %c\n", full, (int)sv.size(), sv[0]);
  return 0;
}
