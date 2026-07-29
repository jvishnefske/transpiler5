// RealWorld C++ corpus (FR-46), project `fixed-stats`: Q16.16 helpers.
// See fixed.hpp for why this project is shaped the way it is.
#include "fixed.hpp"

namespace fixed {

int from_int(int v) { return v * 65536; }

int to_int_round(int q) { return (q + 32768) / 65536; }

int mul(int a, int b) {
  long long p = (long long)a * (long long)b;
  return (int)(p / 65536);
}

int div_count(int total_q, int count) {
  if (count == 0)
    return 0;
  return total_q / count;
}

} // namespace fixed
