// REQUIRES: cargo
// FR-73: differential end-to-end test for the per-TU-prefix underscore
// fold. `_set` is the motivating tinycrypt memset-alike as a FILE-STATIC:
// its emitted name folds `tu0_` + `_set` to `tu0_set` (previously the
// composition manufactured `tu0__set`, which trips the crate's denied
// non_snake_case lint and fails the whole build — the FR-71/72 e2e
// fixtures had to keep their helpers non-static to dodge it). `__mix`
// pins the multiple-leading-underscore collapse and the static global
// `_seed` the global-side fold. The crate must BUILD under the deny set
// and produce the SAME stdout as clang compiling the original natively,
// with every stored value derived from argc so constant folding cannot
// pre-compute the buffer and hide a miscompile. `cargo build` success
// alone proves nothing here — the stdout diff is the oracle.
// Deterministic, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/static_underscore_helper > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern int printf(const char *, ...);

static int _seed = 3;

static void _set(unsigned char *to, unsigned char val, unsigned len) {
  unsigned i;
  for (i = 0; i < len; i++)
    to[i] = val;
}

static int __mix(int a, int b) { return a * 7 + b; }

int main(int argc, char **argv) {
  unsigned char buf[8];
  unsigned i;
  _set(buf, (unsigned char)(argc + _seed), 8u);
  _set(buf, (unsigned char)(argc * 3), 4u);
  for (i = 0; i < 8u; i++)
    printf("%u ", (unsigned)buf[i]);
  printf("\n");
  printf("mix=%d\n", __mix(argc, _seed));
  return 0;
}
