// Companion SB for ../link-merge-anon-struct.c: the second reader of the
// shared header, with its own wrapper around the same anonymous shape.
#include "link-merge-anon-shared.h"

struct WrapB {
  struct {
    int p;
    int q;
  } pt;
};

int shared_b(int v) {
  struct SharedW w;
  struct WrapB b;
  w.pt.p = v * 2;
  w.pt.q = v * 3;
  b.pt.p = w.pt.p;
  b.pt.q = w.pt.q;
  return b.pt.p - b.pt.q;
}
