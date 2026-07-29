// RealWorld C++ corpus (FR-46), project `fixed-stats`: driver TU.
//
// Exercises: `namespace`-qualified free-function calls across a TU boundary,
// a namespace-qualified plain struct as a local, pointer-parameter mutation,
// an `extern "C"` declaration alongside `<cstdio>`, and `long long` scalar
// widening inside the fixed-point multiply. Chosen as the deliberately
// NEAR-IN-SUBSET member of the corpus: everything here is a construct the
// W2.0 wave landed, so it measures whether the supported subset actually
// composes on a realistic multi-TU program rather than on a feature fixture.
#include <cstdio>

#include "fixed.hpp"
#include "stats.hpp"

extern "C" int scaled_reading(int i);

int main(void) {
  stats::Accum a;
  stats::reset(&a);

  for (int i = 0; i < 16; i = i + 1)
    stats::push(&a, fixed::from_int(scaled_reading(i)));

  int mean_q = stats::mean(&a);
  int spread_q = stats::spread(&a);
  int scaled = fixed::mul(mean_q, fixed::from_int(3));

  printf("n=%d mean=%d spread=%d tripled=%d\n", a.count,
         fixed::to_int_round(mean_q), fixed::to_int_round(spread_q),
         fixed::to_int_round(scaled));
  return 0;
}
