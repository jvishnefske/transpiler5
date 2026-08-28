/* FR-139: the C host of the c-abi-exports dlopen end-to-end test -- a stand-in
   for the TRACTOR corpus's `cando` harness, which dlopens `lib<NAME>.so` and
   dlsyms a BARE C symbol, then calls it through a plain C signature.

   It is deliberately built with clang and knows nothing about Rust: the only
   contract is the C ABI. Every dlsym is checked, so an emitted crate that
   exports Rust-mangled `pub fn` (or that never became a cdylib at all) fails
   loudly here instead of producing a diffable but meaningless empty output.

   The function pointers are named exactly like the C functions so that the
   shared driver body compiles unchanged in both legs. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static int (*scale)(int, int);
static unsigned (*crc_step)(unsigned, unsigned char);
static long long (*widen)(int, int);
static double (*blend)(double, float);
static int (*mix6)(int, int, int, int, int, int);
static int (*mix8)(int, int, int, int, int, int, int, int);
static double (*mixfd)(int, double, int, float, int);

#include "c-abi-exports-dlopen-driver.h"

static void *must_sym(void *handle, const char *name) {
  void *sym = dlsym(handle, name);
  if (sym == NULL) {
    fprintf(stderr, "dlsym('%s') failed: %s\n", name, dlerror());
    exit(1);
  }
  return sym;
}

int main(int argc, char **argv) {
  void *handle;
  if (argc < 2) {
    fprintf(stderr, "usage: host <shared-library> [seed words...]\n");
    return 1;
  }
  handle = dlopen(argv[1], RTLD_NOW);
  if (handle == NULL) {
    fprintf(stderr, "dlopen('%s') failed: %s\n", argv[1], dlerror());
    return 1;
  }
  *(void **)&scale = must_sym(handle, "scale");
  *(void **)&crc_step = must_sym(handle, "crc_step");
  *(void **)&widen = must_sym(handle, "widen");
  *(void **)&blend = must_sym(handle, "blend");
  *(void **)&mix6 = must_sym(handle, "mix6");
  *(void **)&mix8 = must_sym(handle, "mix8");
  *(void **)&mixfd = must_sym(handle, "mixfd");
  /* argv[1] is the library path the native leg never sees, so the seed drops
     it: `host <lib>` and `native` both run with seed 1, `host <lib> a b` and
     `native a b` both with seed 3. */
  run_driver(argc - 1);
  return 0;
}
