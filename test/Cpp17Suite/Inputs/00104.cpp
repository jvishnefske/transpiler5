// Cpp17Suite 00104: day-one PASS -- W2.0 subset: nested namespaces, an
// NSDMI-initialized struct (non-trivial default ctor applied field-wise),
// and an explicitly defaulted default ctor next to a user ctor. Only
// NSDMI-initialized fields of `p` are read (an uninitialized field would
// be indeterminate in C++ and the diff meaningless).
extern "C" int printf(const char *, ...);

namespace geo {
struct Point {
  int x = 3;
  int y = -4;
};
namespace detail {
int scale(int v) { return v * 10; }
} // namespace detail
} // namespace geo

struct Tagged {
  int id;
  Tagged(int v) { id = v; }
  Tagged() = default;
};

int main() {
  geo::Point p;
  p.x += 1;
  Tagged t(7);
  printf("%d %d %d\n", p.x, p.y, geo::detail::scale(t.id));
  return 0;
}
