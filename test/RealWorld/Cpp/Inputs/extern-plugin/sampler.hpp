// RealWorld C++ corpus (FR-46/FR-52), project `extern-plugin`: the sampler
// API. Every function here reaches the platform layer, directly or through a
// sibling, so every one of them becomes generic over the emitted trait.
#ifndef EXTERN_PLUGIN_SAMPLER_HPP
#define EXTERN_PLUGIN_SAMPLER_HPP

namespace sampler {

/// A channel's running state: the last raw reading, when it was taken, and
/// how many readings have been folded in.
struct Channel {
  int id;
  int last_raw;
  int last_ms;
  int count;
};

/// Binds `c` to hardware channel `id` and clears its history.
void open(Channel *c, int id);

/// Takes one reading, stamping it with the host clock.
void sample(Channel *c);

/// Takes `n` readings and returns the last one, faulting when `n` is not
/// positive.
int burst(Channel *c, int n);

} // namespace sampler

#endif
