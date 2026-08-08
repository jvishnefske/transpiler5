// Cpp17Suite 00901: frontier marker (corpus-only, no task) -- virtual
// dispatch through a base pointer. Deterministic output so the day this
// transpiles it classifies PASS, not MISCOMPILE; until then W2.0's
// base-class rejection keeps it UNSUPPORTED.
extern "C" int printf(const char *, ...);

struct Shape {
  virtual int sides() const { return 0; }
  virtual ~Shape() {}
};

struct Tri : Shape {
  int sides() const override { return 3; }
};

int main() {
  Tri t;
  Shape *s = &t;
  printf("%d %d\n", s->sides(), t.sides());
  return 0;
}
