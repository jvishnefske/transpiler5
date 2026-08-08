// Cpp17Suite 00502: task-005 target -- std::pair returned by value from a
// function (probes the MaterializeTemporaryExpr path on the returned
// temporary that the task-005 spike must clear).
extern "C" int printf(const char *, ...);

#include <utility>

std::pair<int, int> divmod(int num, int den) {
  return std::pair<int, int>(num / den, num % den);
}

int main() {
  std::pair<int, int> qr = divmod(17, 5);
  printf("%d %d\n", qr.first, qr.second);
  return 0;
}
