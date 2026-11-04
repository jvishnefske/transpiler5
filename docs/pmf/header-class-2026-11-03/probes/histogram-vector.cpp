#include <cstdio>
#include <vector>
#include <cstddef>
// differential probe: Histogram/EntropySource bodies verbatim, with int data_[18] -> std::vector<int>
struct Histogram {
  void AddHistogram(const Histogram& other);
  void Add(size_t val);
  void Merge(const Histogram& other);
  std::vector<int> data_;
  int total_count_;
  double bit_cost_;
};
void Histogram::AddHistogram(const Histogram& other) {
  for (int i = 0; i < 18; ++i) {
    data_[i] += other.data_[i];
  }
  total_count_ += other.total_count_;
}
void Histogram::Add(size_t val) {
  ++data_[val];
  ++total_count_;
}
void Histogram::Merge(const Histogram& other) {
  if (other.total_count_ == 0) return;
  total_count_ += other.total_count_;
  for (size_t i = 0; i < 18; ++i)
    data_[i] += other.data_[i];
}
struct EntropySource {
  void AddCode(size_t code, size_t histo_ix);
  void Merge(const EntropySource& other);
  size_t num_bands_;
  std::vector<Histogram> histograms_;
};
void EntropySource::AddCode(size_t code, size_t histo_ix) {
  histograms_[histo_ix].Add(code);
}
void EntropySource::Merge(const EntropySource& other) {
  for (size_t i = 0; i < other.histograms_.size(); ++i) {
    histograms_[i].Merge(other.histograms_[i]);
  }
}
int main() {
  std::vector<int> z(18, 0);
  Histogram a = {z, 0, 0.0};
  a.Add(3);
  Histogram b = {z, 0, 0.0};
  b.Merge(a);
  std::vector<Histogram> hs(2, a);
  EntropySource s = {2, hs};
  s.AddCode(4, 1);
  EntropySource t = {2, hs};
  t.AddCode(7, 0);
  s.Merge(t);
  b.AddHistogram(a);
  printf("%d %d %d %d\n", b.total_count_, b.data_[3], s.histograms_[1].total_count_, s.histograms_[1].data_[4]);
  printf("%d %d %d\n", s.histograms_[0].total_count_, s.histograms_[0].data_[7], b.data_[3]);
  return 0;
}
