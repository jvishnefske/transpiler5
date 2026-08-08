// Cpp17Suite 00601: task-006 target (R1 probe) -- structured bindings
// over std::pair. Until task-006 lands this must be a LOCATED rejection,
// never silent wrong code: DecompositionDecl IS-A VarDecl, so an unguarded
// DeclStmt path could mis-import it silently.
extern "C" int printf(const char *, ...);

#include <utility>

int main() {
  std::pair<int, int> p(6, 9);
  auto [q, r] = p;
  printf("%d %d\n", q, r);
  return 0;
}
