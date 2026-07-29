// RealWorld C++ corpus (FR-46/FR-51), project `ringbuf-lib`: a small
// order-sensitive checksum over the ring's contents. See ring.hpp for why
// this project has no `main`.
#ifndef RINGBUF_LIB_CHECKSUM_HPP
#define RINGBUF_LIB_CHECKSUM_HPP

#include "ring.hpp"

namespace checksum {

/// Folds one value into a running checksum. Order-sensitive on purpose, so a
/// caller cannot reorder the ring and get the same answer.
int fold(int acc, int value);

/// The checksum of every live sample of `r`, newest first.
int of_ring(const ring::Ring *r);

} // namespace checksum

#endif
