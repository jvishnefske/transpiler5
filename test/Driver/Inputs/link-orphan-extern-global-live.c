// Companion for test/Driver/link-orphan-extern-global.c: the extern global
// `vl_live` is read from a body that is NOT rejected, so the obligation has a
// surviving IR use and must keep the hard "unresolved external" link error.
// Not discovered as a test itself (lives in Inputs/).
struct Iface { int x; };
extern const struct Iface vl_live;

int use_it(int n) { return vl_live.x + n; }

int main(void) { return use_it(1); }
