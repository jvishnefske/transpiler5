// Cpp17Suite 00103: day-one PASS -- W2.2 subset: non-virtual methods, a
// user constructor with a member-initializer list, and overload dispatch
// by both arity (one int vs two ints) and type (int vs bool).
extern "C" int printf(const char *, ...);

class Counter {
public:
  int total;
  Counter(int start) : total(start) {}
  int add(int x) {
    total += x;
    return total;
  }
  int add(int x, int y) {
    total += x + y;
    return total;
  }
  int add(bool flag) { return flag ? total : -total; }
};

int main() {
  Counter c(10);
  int r1 = c.add(5);
  int r2 = c.add(2, 3);
  int r3 = c.add(true);
  int r4 = c.add(false);
  printf("%d %d %d %d\n", r1, r2, r3, r4);
  return 0;
}
