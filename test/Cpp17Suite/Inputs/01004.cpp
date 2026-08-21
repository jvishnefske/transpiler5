extern "C" int printf(const char *, ...);

struct Base {
  int x;
  Base(int v) : x(v) {}
  int get() const { return x; }
  void bump(int d) { x += d; }
};

struct Derived : Base {
  int y;
  Derived(int a, int b) : Base(a), y(b) {}
  int sum() const { return get() + y; }
  int scaled() const { return x * 3 + y; }
  void grow(int d) { x += d; bump(d); y += d; }
  int viaThis() const { return this->get() + this->x; }
  int viaQualified() const { return Base::get(); }
};

struct Third : Derived {
  int z;
  Third(int a, int b, int c) : Derived(a, b), z(c) {}
  int total() const { return get() + x + y + z; }
};

int main() {
  Derived d(3, 4);
  printf("%d\n", d.sum());
  printf("%d\n", d.scaled());
  d.grow(2);
  printf("%d %d\n", d.get(), d.y);
  printf("%d\n", d.viaThis());
  printf("%d\n", d.viaQualified());
  Third t(10, 20, 30);
  printf("%d\n", t.total());
  printf("%d\n", t.sum());
  return 0;
}
