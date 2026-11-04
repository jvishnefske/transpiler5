#include <cstdio>
struct S {
  int get() const;
  int x;
  int* p;
};
int S::get() const { return x; }
int user(const S& s) { return s.get() + s.x; }
int main() { return 0; }
