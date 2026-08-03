// Subsystem B of the FR-59 workspace partition test: depends on subsystem
// A (calls two of its functions and names its record in a local), so the
// emitted crate carries a path dependency on subsys_a and a `use
// subsys_a::*;` import.

#include "../subsys_a/pair.h"

int pair_sum(struct Pair p);
int make_val(int x);

int calc(int n) {
  struct Pair p;
  p.x = make_val(n);
  p.y = n;
  return pair_sum(p);
}
