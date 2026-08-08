// Cpp17Suite 00602: task-006 target (R1 probe) -- structured bindings
// over a user struct's public fields.
extern "C" int printf(const char *, ...);

struct Dim {
  int w;
  int h;
};

int main() {
  Dim d;
  d.w = 12;
  d.h = 5;
  auto [w, h] = d;
  printf("%d %d %d\n", w, h, w * h);
  return 0;
}
