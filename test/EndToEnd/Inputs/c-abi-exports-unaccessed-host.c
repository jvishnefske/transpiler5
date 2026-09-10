/* FR-226: the C host of the c-abi-exports-unaccessed dlopen end-to-end test.

   It is deliberately built with clang and knows nothing about Rust: the only
   contract is the C ABI. Every dlsym is checked, so an emitted crate that
   exports nothing for these functions -- which is exactly what happened before
   this class existed, and what the TRACTOR harness recorded as SYMBOL_MISSING
   for `SPX_initialize_hash_function` across 12 corpus cases -- fails loudly
   here instead of producing a diffable but meaningless empty output.

   The function pointers are named exactly like the C functions so that the
   shared driver body compiles unchanged in both legs, and their declarations
   are the ORIGINAL C ones: one pointer argument slot, no length beside it. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "c-abi-exports-unaccessed-types.h"

static void (*initialize_hash_function)(spx_ctx *);
static int (*untouched)(const unsigned char *);
static int (*trail)(unsigned char *, int);
static int (*lead)(int, unsigned char *);

#include "c-abi-exports-unaccessed-driver.h"

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
  *(void **)&initialize_hash_function =
      must_sym(handle, "initialize_hash_function");
  *(void **)&untouched = must_sym(handle, "untouched");
  *(void **)&trail = must_sym(handle, "trail");
  *(void **)&lead = must_sym(handle, "lead");
  /* argv[1] is the library path the native leg never sees, so the seed drops
     it: `host <lib>` and `native` both run with seed 1, `host <lib> a b` and
     `native a b` both with seed 3. */
  run_driver(argc - 1);
  return 0;
}
