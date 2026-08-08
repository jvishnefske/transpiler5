// Cpp17Suite 00302: task-003 target -- string push_back(char) and clear(),
// the string-side siblings of the vector methods task-003 adds.
extern "C" int printf(const char *, ...);

#include <string>

int main() {
  std::string s = "ab";
  s.push_back('c');
  s.push_back('!');
  int len_before = s.length();
  printf("%s %d\n", s.c_str(), len_before);
  s.clear();
  int len_after = s.length();
  int em = s.empty();
  printf("%d %d\n", len_after, em);
  return 0;
}
