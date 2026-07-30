// RealWorld C++ corpus (FR-46/FR-52), project `extern-plugin`: the pure half
// of the project.
//
// Nothing here reaches the platform layer, so nothing here may grow a type
// parameter. That is the load-bearing negative of FR-52's propagation rule --
// only the transitive closure of a requirement's callers changes, and a
// project's untouched code must stay untouched -- and it is why this project
// mixes both kinds of function rather than being all one.
#ifndef EXTERN_PLUGIN_FILTER_HPP
#define EXTERN_PLUGIN_FILTER_HPP

namespace filter {

/// Clamps `v` into [lo, hi].
int clamp(int v, int lo, int hi);

/// A 4-tap boxcar average of `raw` and the three previous values in `hist`.
int boxcar(const int *hist, int raw);

} // namespace filter

#endif
