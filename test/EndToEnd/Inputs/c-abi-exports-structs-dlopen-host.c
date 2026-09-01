/* FR-182: the C host of the c-abi-exports-structs dlopen end-to-end test.

   It is deliberately built with clang and knows nothing about Rust: the only
   contract is the C ABI and the shared record definitions. Every dlsym is
   checked, so an emitted crate that exports Rust-mangled `pub fn` (or that
   never became a cdylib at all) fails loudly here instead of producing a
   diffable but meaningless empty output.

   The function pointers are named exactly like the C functions so that the
   shared driver body compiles unchanged in both legs. Note the shapes they
   span: structs passed and returned BY VALUE in every SysV class (single-SSE
   `vec2`, mixed INTEGER/SSE `wide`, MEMORY-class `big`), a struct argument
   sandwiched between scalars, and the two POINTER entries whose storage this
   host owns. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "c-abi-exports-structs-dlopen-types.h"

static vec2 (*vec_add)(vec2, vec2);
static float (*vec_dot)(vec2, vec2);
static double (*wide_mix)(struct wide, int);
static struct big (*big_shift)(struct big, int);
static int (*big_sum)(struct big);
static int (*mix_after)(int, vec2, int, double, vec2, int);
static int (*tflac_validate)(struct tflac *, int);
static unsigned (*tflac_peek)(struct tflac *);
static float (*span_width)(struct span *);

#include "c-abi-exports-structs-dlopen-driver.h"

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
  *(void **)&vec_add = must_sym(handle, "vec_add");
  *(void **)&vec_dot = must_sym(handle, "vec_dot");
  *(void **)&wide_mix = must_sym(handle, "wide_mix");
  *(void **)&big_shift = must_sym(handle, "big_shift");
  *(void **)&big_sum = must_sym(handle, "big_sum");
  *(void **)&mix_after = must_sym(handle, "mix_after");
  *(void **)&tflac_validate = must_sym(handle, "tflac_validate");
  *(void **)&tflac_peek = must_sym(handle, "tflac_peek");
  *(void **)&span_width = must_sym(handle, "span_width");
  /* argv[1] is the library path the native leg never sees, so the seed drops
     it: `host <lib>` and `native` both run with seed 1, `host <lib> a b` and
     `native a b` both with seed 3. */
  run_driver(argc - 1);
  return 0;
}
