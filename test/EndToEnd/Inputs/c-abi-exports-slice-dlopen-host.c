/* FR-202: the C host of the c-abi-exports-slice dlopen end-to-end test.

   It is deliberately built with clang and knows nothing about Rust: the only
   contract is the C ABI. Every dlsym is checked, so an emitted crate that
   exports Rust-mangled `pub fn` -- which is exactly what this shape did before
   the must-access bound analysis, and what the TRACTOR corpus recorded as
   SYMBOL_MISSING for `hdr_bitrate` -- fails loudly here instead of producing a
   diffable but meaningless empty output.

   The function pointers are named exactly like the C functions so that the
   shared driver body compiles unchanged in both legs. Their C declarations are
   the ORIGINAL C ones (`const unsigned char *`, one pointer argument slot),
   which is the whole claim under test: the emitted wrapper must take that one
   register and manufacture the length itself. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned (*hdr_bitrate)(const unsigned char *);
static int (*byte_pick)(const unsigned char *, int);
static int (*lead_scalar)(int, const unsigned char *);
static unsigned (*bump_read)(const unsigned char *);

#include "c-abi-exports-slice-dlopen-driver.h"

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
  *(void **)&hdr_bitrate = must_sym(handle, "hdr_bitrate");
  *(void **)&byte_pick = must_sym(handle, "byte_pick");
  *(void **)&lead_scalar = must_sym(handle, "lead_scalar");
  *(void **)&bump_read = must_sym(handle, "bump_read");
  /* argv[1] is the library path the native leg never sees, so the seed drops
     it: `host <lib>` and `native` both run with seed 1, `host <lib> a b` and
     `native a b` both with seed 3. */
  run_driver(argc - 1);
  return 0;
}
