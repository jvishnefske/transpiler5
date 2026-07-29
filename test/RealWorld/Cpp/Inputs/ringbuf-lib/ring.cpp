// RealWorld C++ corpus (FR-46/FR-51), project `ringbuf-lib`: ring body.
// See ring.hpp for the shape rationale.
#include "ring.hpp"

namespace ring {

int capacity() { return 8; }

void reset(Ring *r) {
  for (int i = 0; i < 8; i++)
    r->data[i] = 0;
  r->head = 0;
  r->count = 0;
}

void push(Ring *r, int value) {
  r->data[r->head] = value;
  r->head = (r->head + 1) % 8;
  if (r->count < 8)
    r->count = r->count + 1;
}

int size(const Ring *r) { return r->count; }

int sum(const Ring *r) {
  int total = 0;
  for (int i = 0; i < r->count; i++)
    total = total + r->data[i];
  return total;
}

int recent(const Ring *r, int i) {
  if (i < 0 || i >= r->count)
    return 0;
  int index = r->head - 1 - i;
  while (index < 0)
    index = index + 8;
  return r->data[index];
}

} // namespace ring
