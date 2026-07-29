// RealWorld C++ corpus (FR-46), project `fixed-stats`: a streaming statistics
// accumulator over Q16.16 samples. A plain data-only `struct` (a `class`
// keyword would import identically since W2.0) mutated through POINTER
// parameters, never references and never member functions -- the C-shaped
// half of the C++ subset, on purpose.
#ifndef FIXED_STATS_STATS_HPP
#define FIXED_STATS_STATS_HPP

namespace stats {

/// Running count/total/min/max over a stream of Q16.16 samples.
struct Accum {
  int count;
  int total; ///< Q16.16 running sum.
  int min;   ///< Q16.16.
  int max;   ///< Q16.16.
};

/// Resets `a` to the empty stream.
void reset(Accum *a);

/// Folds one Q16.16 sample into `a`.
void push(Accum *a, int sample_q);

/// Q16.16 arithmetic mean, or 0 for an empty stream.
int mean(const Accum *a);

/// Q16.16 max - min, or 0 for an empty stream.
int spread(const Accum *a);

} // namespace stats

#endif
