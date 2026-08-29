// Companion B for ../link-merge-anon-struct.c: a bare anonymous struct whose
// shape (`{long b; long c;}`) DIFFERS from the one in the main file, so a
// content-keyed synthesized name must differ from A's while the
// first-encounter counter gave both `Anon0`.
struct OuterB {
  struct {
    long b;
    long c;
  } inner;
};

long anon_b(long v) {
  struct OuterB o;
  o.inner.b = v;
  o.inner.c = v + 1;
  return o.inner.b + o.inner.c;
}
