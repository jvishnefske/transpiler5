// RealWorld C++ corpus (FR-46/FR-51), project `ringbuf-lib`: checksum body.
//
// `scramble` is a file-`static` helper, and it is here on purpose: FR-51's
// library-crate visibility rule keeps internal-linkage items PRIVATE while
// exporting the rest, so this project exercises both sides of that rule
// rather than only the exported one. In the emitted crate it becomes
// `tu<i>_scramble` with no `pub`, while every function declared in a header
// above becomes `pub`.
#include "checksum.hpp"

namespace checksum {

static int scramble(int v) { return (v * 31) ^ (v / 7); }

int fold(int acc, int value) { return acc * 3 + scramble(value); }

int of_ring(const ring::Ring *r) {
  int acc = 17;
  int live = ring::size(r);
  for (int i = 0; i < live; i++)
    acc = fold(acc, ring::recent(r, i));
  return acc;
}

} // namespace checksum
