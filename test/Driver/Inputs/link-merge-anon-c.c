// Companion C for ../link-merge-anon-struct.c: carries the SAME anonymous
// shape as the main file's (`{int a;}`) but at a DIFFERENT first-encounter
// ordinal, because an unrelated shape (`{short z;}`) is imported ahead of it.
// Under the old per-import counter this file called `{short z;}` Anon0 and
// `{int a;}` Anon1, while the main file called `{int a;}` Anon0 -- the
// cross-invocation instability FR-151 removes.
struct HeadC {
  struct {
    short z;
  } head;
};

struct OuterC {
  struct {
    int a;
  } inner;
};

int anon_c(int v) {
  struct HeadC h;
  struct OuterC o;
  h.head.z = (short)v;
  o.inner.a = v + h.head.z;
  return o.inner.a;
}
