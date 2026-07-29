// RealWorld C++ corpus (FR-46), project `fixed-stats`: Q16.16 fixed-point
// arithmetic helpers. Deliberately NEAR-IN-SUBSET: a `namespace` holding only
// free functions over builtin scalars, no class, no method, no reference, no
// template. Chosen because fixed-point scalar math is what real firmware and
// DSP code actually looks like once floating point is off the table, and it is
// the shape the W2.0 namespace/`extern "C"` import was built for.
#ifndef FIXED_STATS_FIXED_HPP
#define FIXED_STATS_FIXED_HPP

namespace fixed {

// A Q16.16 value: 16 integer bits, 16 fraction bits, held in a plain `int`.
// The scale factor 65536 is spelled literally at each use rather than as a
// namespace-scope constant, matching the header's free-function-only shape.

/// Lifts a whole number into Q16.16.
int from_int(int v);

/// Rounds a Q16.16 value back to the nearest whole number (ties away from
/// zero for positive inputs; the corpus feeds only positive readings).
int to_int_round(int q);

/// Q16.16 product, computed through a 64-bit intermediate so the 32-bit
/// operands cannot overflow before the rescaling shift.
int mul(int a, int b);

/// Q16.16 quotient of a whole-number total by a whole-number count; returns 0
/// for a zero count rather than trapping.
int div_count(int total_q, int count);

} // namespace fixed

#endif
