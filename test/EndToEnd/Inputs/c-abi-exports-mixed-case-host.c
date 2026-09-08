/* FR-208: the C host of the c-abi-exports-mixed-case dlopen end-to-end test.

   It is deliberately built with clang and knows nothing about Rust: the only
   contract is the C ABI and the shared record definition. Every dlsym is
   spelled with the C source's OWN name and every dlsym is checked, so an
   emitted crate that exports the FR-53 idiomatic rename instead of the C
   spelling fails loudly here -- with the missing symbol printed -- rather
   than producing a diffable but meaningless empty output.

   That is exactly what this file pins. At the pre-FR-208 emitter the crate
   exported `spx_add`/`spx_crc_step`/`spx_widen`/`spx_pair_sum`, so
   `dlsym('SPX_add')` returned NULL and this host exited 1.

   The function pointers are named exactly like the C functions so that the
   shared driver body compiles unchanged in all three legs. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "c-abi-exports-mixed-case-types.h"

static int (*SPX_add)(int, int);
static unsigned (*SPX_crcStep)(unsigned, unsigned char);
static long long (*spxWiden)(int, int);
static double (*SPX_blend)(double, float);
static int (*SPX_mix8)(int, int, int, int, int, int, int, int);
static int (*plain_add)(int, int);
static int (*SPX_pair_sum)(spx_pair *);
static void (*SPX_pair_scale)(spx_pair *, int);

#include "c-abi-exports-mixed-case-driver.h"

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
  *(void **)&SPX_add = must_sym(handle, "SPX_add");
  *(void **)&SPX_crcStep = must_sym(handle, "SPX_crcStep");
  *(void **)&spxWiden = must_sym(handle, "spxWiden");
  *(void **)&SPX_blend = must_sym(handle, "SPX_blend");
  *(void **)&SPX_mix8 = must_sym(handle, "SPX_mix8");
  *(void **)&plain_add = must_sym(handle, "plain_add");
  *(void **)&SPX_pair_sum = must_sym(handle, "SPX_pair_sum");
  *(void **)&SPX_pair_scale = must_sym(handle, "SPX_pair_scale");
  /* The emitted crate must NOT still be carrying the FR-53 rename. A stale
     alias would let the diff pass while the contract stayed broken, so the
     renamed spellings are asserted ABSENT rather than merely unused. */
  if (dlsym(handle, "spx_add") != NULL ||
      dlsym(handle, "spx_crc_step") != NULL ||
      dlsym(handle, "spx_widen") != NULL ||
      dlsym(handle, "spx_pair_sum") != NULL) {
    fprintf(stderr, "the idiomatic rename is still exported\n");
    return 1;
  }
  /* argv[1] is the library path the native leg never sees, so the seed drops
     it: `host <lib>` and `native` both run with seed 1, `host <lib> a b` and
     `native a b` both with seed 3. */
  run_driver(argc - 1);
  return 0;
}
