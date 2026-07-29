// RealWorld C++ corpus (FR-46/FR-51), project `ringbuf-lib`: a third
// translation unit that composes the other two, so the project is genuinely
// multi-TU rather than two independent files. It declares its own API in this
// file (there is no frame.hpp) purely to keep the header count honest: the
// project has headers where headers earn their keep and not otherwise.
#include "checksum.hpp"
#include "ring.hpp"

namespace frame {

/// Summarizes `r` as a single integer keyed by `seq`: the checksum of its
/// contents mixed with its running total and the sequence number. This is the
/// project's top-level entry point in the ordinary library sense -- a caller
/// links against the crate and calls it -- which is exactly the thing a crate
/// with no `fn main` has to be able to export.
int encode(const ring::Ring *r, int seq) {
  int mixed = checksum::of_ring(r) + ring::sum(r) * 5;
  return mixed ^ (seq * 131);
}

/// True (1) when `r` holds every sample it can.
int is_full(const ring::Ring *r) {
  return ring::size(r) == ring::capacity() ? 1 : 0;
}

} // namespace frame
