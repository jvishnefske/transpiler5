// Cpp17Suite 00102: day-one PASS -- std::string usage inside the W2.3
// subset (literal ctor, += char, += literal, length/size, empty, c_str fed
// straight to printf's %s).
extern "C" int printf(const char *, ...);

#include <string>

int main() {
  std::string s = "cpp17";
  s += ':';
  s += "suite";
  int len = s.length();
  int sz = s.size();
  int em = s.empty();
  printf("%s len=%d size=%d empty=%d\n", s.c_str(), len, sz, em);
  return 0;
}
