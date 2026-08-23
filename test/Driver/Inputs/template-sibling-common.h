struct Nasty {
  int v;
  Nasty();
  Nasty(const Nasty &o);
};
template <typename T> T pass(T x) { return x; }
