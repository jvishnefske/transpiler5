// RealWorld C++ corpus (FR-46/FR-51), project `ringbuf-lib`: a fixed-capacity
// ring buffer of samples.
//
// WHY THIS PROJECT EXISTS. Every other project in this corpus has a `main`,
// so every crate emitted from it is a BINARY crate. That made an entire shape
// of real code unreachable by the corpus: a LIBRARY, which is what most C and
// C++ actually is. `ringbuf-lib` defines no `main` in any translation unit,
// so it exercises FR-51's library-crate path end to end -- `src/lib.rs`, the
// `[lib]` manifest section, and the `pub` visibility rule -- and it scores
// LIB_BUILT rather than TRANSPILED because there is nothing to run.
//
// The code is deliberately the same C-shaped subset `fixed-stats` uses: plain
// data-only structs mutated through POINTER parameters, free functions inside
// a `namespace`, no member functions, no references, no templates. The point
// of the project is its SHAPE (no entry point), not new input constructs.
#ifndef RINGBUF_LIB_RING_HPP
#define RINGBUF_LIB_RING_HPP

namespace ring {

/// A fixed-capacity ring of 8 samples. `head` is the write cursor and `count`
/// saturates at the capacity, so a full ring overwrites its oldest sample.
struct Ring {
  int data[8];
  int head;
  int count;
};

/// Empties `r`, zeroing every slot so the buffer has no stale samples.
void reset(Ring *r);

/// Writes `value` at the cursor, advancing it and growing `count` until the
/// ring is full.
void push(Ring *r, int value);

/// The number of slots in a `Ring`, as a value rather than a macro.
int capacity();

/// The number of live samples in `r`.
int size(const Ring *r);

/// The sum of the live samples in `r`, or 0 when it is empty.
int sum(const Ring *r);

/// The `i`th live sample of `r` counting back from the newest (`i == 0` is
/// the most recently pushed), or 0 when `i` is out of range.
int recent(const Ring *r, int i);

} // namespace ring

#endif
