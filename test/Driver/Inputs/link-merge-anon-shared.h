// Shared header for ../link-merge-anon-struct.c: ONE anonymous shape reached
// from two translation units. Both shards must name it identically, so the
// merge dedups it to a single struct and `struct SharedW` stays structurally
// equal between the shards.
struct SharedW {
  struct {
    int p;
    int q;
  } pt;
};
