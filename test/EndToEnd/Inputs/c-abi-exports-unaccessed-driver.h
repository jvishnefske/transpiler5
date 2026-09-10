/* FR-226: the ONE driver body of the c-abi-exports-unaccessed dlopen
   end-to-end test, compiled twice so the two legs cannot drift apart.

   In the NATIVE leg it is included by the library source itself, so every call
   binds to the C function directly. In the dlopen leg it is included by the
   host after the host has declared file-scope FUNCTION POINTERS under the very
   same names and filled them from dlsym, so the identical call syntax binds to
   the emitted cdylib's bare C symbols. A difference in the printed bytes can
   therefore only come from the CROSSING.

   Every value fed in derives from `seed`, which derives from argc, so nothing
   here can be constant-folded on either side.

   WHAT THE PRINTED BYTES ARE FOR. The exported class promises that the callee
   NEVER TOUCHES the pointer, and the wrapper discharges that by dropping the
   pointer and passing an empty slice of its own. Three things would break it,
   and each has a line here:

     * a wrapper that wrote through the pointer, or that built a real slice
       from it and let the callee write -- caught by dumping all 36 bytes of
       the caller's `spx_ctx` after the call and diffing them against the
       native's;
     * a wrapper that called `from_raw_parts` -- caught by passing NULL, which
       is the exact case the refused-zero-bound comment worried about. A
       `from_raw_parts(NULL, 0)` is instant UB and a real dereference is a
       SIGSEGV, so this line is a crash-or-pass, not a wrong number;
     * a wrapper that let the fat pointer's LENGTH consume an argument
       register -- caught by `trail` and `lead`, which put a seed-derived
       scalar after and before the pointer. FR-181 measured exactly that shift
       as native 21983 against export 25322, exit 0, no diagnostic.

   The WILD pointer is `4 * (seed + 1)`: non-null, a multiple of the record's
   alignment so that forming it is implementation-defined rather than undefined
   in the C input, and certainly not an object either leg owns. Reading one
   byte through it would be a SIGSEGV in both legs. */

#include <stdio.h>

static void run_driver(int seed) {
  spx_ctx ctx;
  unsigned char probe[4];
  int i;

  for (i = 0; i < 32; ++i)
    ctx.seed[i] = (unsigned char)(seed * 13 + i * 7 + 1);
  ctx.n = seed * 31 + 5;

  /* A REAL object, fully initialized, handed across and expected back
     untouched. The corpus harness's own vectors assert exactly this: its
     `lib_state_in` equals its `lib_state_out`, because the C changes
     nothing. */
  initialize_hash_function(&ctx);
  printf("ctx.n=%d\n", ctx.n);
  for (i = 0; i < 32; ++i)
    printf("ctx.seed[%d]=%u\n", i, (unsigned)ctx.seed[i]);

  /* NULL, which a C caller may legally pass for a pointer nothing reads. */
  initialize_hash_function((spx_ctx *)0);
  printf("null_ok=%d\n", seed);

  /* A wild but correctly aligned address: not merely null, but an address no
     mapping backs. Nothing may be loaded from it. */
  initialize_hash_function((spx_ctx *)(unsigned long)(4u * (unsigned)(seed + 1)));
  printf("wild_ok=%d\n", seed);

  /* The shared-slice half of the class, called with NULL and with a real
     buffer, so the const spelling is exercised on both. */
  printf("untouched_null=%d\n", untouched((const unsigned char *)0));
  for (i = 0; i < 4; ++i)
    probe[i] = (unsigned char)(seed * 11 + i * 29 + 2);
  printf("untouched_buf=%d\n", untouched(probe));

  /* The argument-slot oracles. */
  printf("trail=%d\n", trail(probe, seed * 3 + 1));
  printf("lead=%d\n", lead(seed * 5, probe));
  printf("trail_null=%d\n", trail((unsigned char *)0, seed * 7 + 2));
  printf("lead_null=%d\n", lead(seed * 9, (unsigned char *)0));
}
