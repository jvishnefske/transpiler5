// Cpp17Suite 01009: virtual methods on VALUES only (W2.19a) -- a base
// with virtual methods under derived classes that override SOME of them
// and inherit the rest, with `override`, `final`, and a three-level
// hierarchy, every object a local VALUE. Deterministic stdout that makes
// the static binding observable: on a value C++'s static type IS its
// dynamic type, so every call must bind the receiver's own type's
// override -- `d.noise()` prints Dog's 2xx line, not Animal's 1xx -- and
// a virtual method the derived class does NOT override must bind the
// base's body through the W2.18 base-field hop (`d.weight()` reads
// Animal::weight through `d.base`). No pointers, no `new`, no upcasts:
// every DYNAMIC channel stays a located rejection (00901 needs W2.19b).
extern "C" int printf(const char *, ...);

struct Animal {
  int legs;
  Animal(int l) : legs(l) {}
  virtual int noise() { return 100 + legs; }
  virtual int weight() { return 10 + legs; }
  int tag() { return legs * 7; }
};

struct Dog : Animal {
  int bark;
  Dog(int l, int b) : Animal(l), bark(b) {}
  int noise() override { return 200 + bark + legs; }
};

struct Puppy : Dog {
  Puppy(int l, int b) : Dog(l, b) {}
  int noise() override final { return 300 + bark; }
  int weight() override { return 3 + legs; }
};

int main() {
  Animal a(4);
  Dog d(2, 5);
  Puppy p(1, 9);
  printf("%d %d %d\n", a.noise(), a.weight(), a.tag());
  printf("%d %d %d\n", d.noise(), d.weight(), d.tag());
  printf("%d %d %d\n", p.noise(), p.weight(), p.tag());
  return 0;
}
