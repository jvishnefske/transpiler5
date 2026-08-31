/* FR-179 (FR-139 follow-on): the C host of the actor-lift dlopen end-to-end
   test -- the same shape as the FR-139 host next to it, and the same shape
   the TRACTOR corpus's `cando` harness uses: dlopen `lib<NAME>.so`, dlsym a
   BARE C symbol, call it through a plain C declaration.

   Built with clang; it knows nothing about Rust and nothing about the FR-62
   actor lift. If the emitted crate kept the lifted functions as Rust-mangled
   `pub fn` methods -- which is what it did before the wrapper existed -- every
   dlsym here returns null and the test fails loudly instead of diffing an
   empty output against a full one. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned short (*encode)(int);
static int (*acc_add)(int);
static int (*acc_get)(void);

#include "c-abi-exports-actor-driver.h"

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
  *(void **)&encode = must_sym(handle, "encode");
  *(void **)&acc_add = must_sym(handle, "acc_add");
  *(void **)&acc_get = must_sym(handle, "acc_get");
  /* argv[1] is the library path the native leg never sees, so the seed drops
     it: `host <lib>` and `native` both run with seed 1. */
  run_driver(argc - 1);
  return 0;
}
