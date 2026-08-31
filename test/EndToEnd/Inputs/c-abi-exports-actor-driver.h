/* FR-179 (FR-139 follow-on): the ONE driver body of the actor-lift dlopen
   end-to-end test, compiled twice so the two legs cannot drift apart.

   In the NATIVE leg it is included by the library source itself, so every
   call binds to the C function directly and the file-scope statics behave
   the way C says. In the dlopen leg it is included by the host after the
   host has declared file-scope FUNCTION POINTERS under the same names and
   filled them from dlsym, so the identical call syntax binds to the emitted
   cdylib's bare C symbols -- which, for these functions, are the FR-139
   wrappers around a thread_local owner singleton.

   The SEQUENCE is the point. `acc_add` mutates lifted state and `acc_get`
   reads it back, both through their own wrappers on the SAME owner, so a
   wrapper that constructed a fresh actor per call (or one singleton per
   wrapper) would agree with the native leg on the first call and diverge on
   the second. Every value fed in derives from `seed`, which derives from
   argc, so nothing on either side can be constant-folded. */

static void run_driver(int seed) {
  int i;
  for (i = 0; i < 8; ++i)
    printf("encode=%u\n", encode(seed * 3 + i));
  for (i = 0; i < 6; ++i)
    printf("acc_add=%d\n", acc_add(seed * i + 1));
  printf("acc_get=%d\n", acc_get());
  printf("acc_add=%d\n", acc_add(-seed));
  printf("acc_get=%d\n", acc_get());
  for (i = 0; i < 4; ++i)
    printf("encode=%u\n", encode(seed + i * 5));
  printf("acc_get=%d\n", acc_get());
}
