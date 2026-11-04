#include <cstdio>
enum Color { R, G, B };            // no fixed type: valid values 0..3
enum Fixed : int { X, Y };         // fixed: every int is a valid value
int id(int v) { return v; }
int main(int argc, char**) {
  Fixed f = static_cast<Fixed>(id(42));
  printf("%d\n", (int)f);
  Color c = static_cast<Color>(id(42 + argc - 1));   // UB (CWG1766): 42 is outside 0..3
  printf("%d\n", (int)c);
  return 0;
}
