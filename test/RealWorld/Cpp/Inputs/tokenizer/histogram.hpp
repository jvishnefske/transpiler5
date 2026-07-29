// RealWorld C++ corpus (FR-46), project `tokenizer`: a fixed-bin histogram.
// The second class of the project, chosen to add a FIXED ARRAY data member
// (rather than a pointer or a container) to the method surface: array member
// + method receiver together, which no single-class fixture covers.
#ifndef TOKENIZER_HISTOGRAM_HPP
#define TOKENIZER_HISTOGRAM_HPP

/// Counts word lengths into 8 fixed bins; lengths >= 8 saturate into bin 7.
class Histogram {
public:
  Histogram();

  /// Adds one observation of `length`.
  void add(int length);

  int bin(int index) const;
  int total() const;
  /// Index of the most populated bin (lowest index wins a tie).
  int peak_bin() const;

private:
  int bins_[8];
  int total_;
};

#endif
