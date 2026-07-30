// RealWorld C++ corpus (FR-46/FR-52), project `extern-plugin`: sampler
// bodies. `open` and `sample` call the platform layer directly; `burst`
// reaches it only through `sample` and `platform::fault`, so it covers the
// TRANSITIVE half of the propagation rule.
#include "sampler.hpp"

#include "platform.hpp"

namespace sampler {

void open(Channel *c, int id) {
  c->id = id;
  c->last_raw = platform::read_raw(id);
  c->last_ms = platform::millis();
  c->count = 1;
}

void sample(Channel *c) {
  c->last_raw = platform::read_raw(c->id);
  c->last_ms = platform::millis();
  c->count = c->count + 1;
}

int burst(Channel *c, int n) {
  if (n <= 0) {
    platform::fault(1);
    return 0;
  }
  for (int i = 0; i < n; i++)
    sample(c);
  return c->last_raw;
}

} // namespace sampler
