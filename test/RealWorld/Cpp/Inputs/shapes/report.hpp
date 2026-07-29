// RealWorld C++ corpus (FR-46), project `shapes`: the dispatch loop, split
// into its own TU so virtual dispatch happens ACROSS a translation-unit
// boundary -- the base class is known only by declaration at the call site,
// which is exactly the situation that makes devirtualization impossible and
// therefore forces a real vtable.
#ifndef SHAPES_REPORT_HPP
#define SHAPES_REPORT_HPP

class Shape;

/// Prints one line per shape and returns the total area (in hundredths).
int report(Shape *const *shapes, int count);

#endif
