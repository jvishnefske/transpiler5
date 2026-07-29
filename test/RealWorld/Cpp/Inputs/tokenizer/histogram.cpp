// RealWorld C++ corpus (FR-46), project `tokenizer`: histogram body.
// See histogram.hpp for the shape rationale.
#include "histogram.hpp"

Histogram::Histogram() : total_(0) {
  for (int i = 0; i < 8; i = i + 1)
    bins_[i] = 0;
}

void Histogram::add(int length) {
  int index = length;
  if (index < 0)
    index = 0;
  if (index > 7)
    index = 7;
  bins_[index] = bins_[index] + 1;
  total_ = total_ + 1;
}

int Histogram::bin(int index) const {
  if (index < 0 || index > 7)
    return 0;
  return bins_[index];
}

int Histogram::total() const { return total_; }

int Histogram::peak_bin() const {
  int best = 0;
  for (int i = 1; i < 8; i = i + 1) {
    if (bins_[i] > bins_[best])
      best = i;
  }
  return best;
}
