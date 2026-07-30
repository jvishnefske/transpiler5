// RealWorld C++ corpus (FR-46/FR-52), project `extern-plugin`: filter bodies.
// Deliberately self-contained -- see filter.hpp.
#include "filter.hpp"

namespace filter {

static int taps() { return 4; }

int clamp(int v, int lo, int hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

int boxcar(const int *hist, int raw) {
  int acc = raw;
  int n = taps();
  for (int i = 0; i < n - 1; i++)
    acc = acc + hist[i];
  return acc / n;
}

} // namespace filter
