// RealWorld C++ corpus (FR-46), project `fixed-stats`: accumulator body.
// See stats.hpp for the shape rationale.
#include "stats.hpp"
#include "fixed.hpp"

namespace stats {

void reset(Accum *a) {
  a->count = 0;
  a->total = 0;
  a->min = 0;
  a->max = 0;
}

void push(Accum *a, int sample_q) {
  if (a->count == 0) {
    a->min = sample_q;
    a->max = sample_q;
  } else {
    if (sample_q < a->min)
      a->min = sample_q;
    if (sample_q > a->max)
      a->max = sample_q;
  }
  a->total = a->total + sample_q;
  a->count = a->count + 1;
}

int mean(const Accum *a) { return fixed::div_count(a->total, a->count); }

int spread(const Accum *a) {
  if (a->count == 0)
    return 0;
  return a->max - a->min;
}

} // namespace stats
