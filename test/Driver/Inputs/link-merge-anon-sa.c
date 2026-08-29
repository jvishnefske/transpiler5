// Companion SA for ../link-merge-anon-struct.c: reaches the shared header's
// one anonymous shape, and declares its OWN wrapper around an identical
// shape, so the merge has to name the same shape identically both through a
// shared header and through two independently written wrappers.
#include "link-merge-anon-shared.h"

struct WrapA {
  struct {
    int p;
    int q;
  } pt;
};

int shared_a(int v) {
  struct SharedW w;
  struct WrapA a;
  w.pt.p = v;
  w.pt.q = v + 1;
  a.pt.p = w.pt.p;
  a.pt.q = w.pt.q;
  return a.pt.p + a.pt.q;
}
