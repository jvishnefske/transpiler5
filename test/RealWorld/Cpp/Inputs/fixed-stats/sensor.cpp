// RealWorld C++ corpus (FR-46), project `fixed-stats`: the sensor stub TU.
// Declared `extern "C"` so the emitted symbol keeps plain C linkage naming --
// the other half of the W2.0 `LinkageSpecDecl` walk that `namespace` exercises
// from the opposite direction. A deterministic pseudo-reading so the program
// has no clock, no randomness, and no input.
extern "C" int scaled_reading(int i) {
  int base = 20 + ((i * 7) % 13) - 6;
  if (base < 0)
    base = 0;
  return base;
}
