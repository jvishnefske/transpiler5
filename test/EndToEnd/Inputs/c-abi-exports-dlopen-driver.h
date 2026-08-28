/* FR-139: the ONE driver body of the c-abi-exports dlopen end-to-end test,
   compiled twice so that the two legs cannot drift apart.

   In the NATIVE leg it is included by the library source itself, so every
   call below binds to the C function directly. In the dlopen leg it is
   included by the host after the host has declared file-scope FUNCTION
   POINTERS under the very same names and filled them from dlsym, so the
   identical call syntax binds to the emitted cdylib's bare C symbols.
   Keeping the sequence in one file is what makes the byte-diff meaningful:
   a difference in the output can only come from the crossing, never from
   two hand-copied drivers falling out of step.

   Every value fed in derives from `seed`, which derives from argc, so no
   result can be constant-folded on either side. */

static void run_driver(int seed) {
  unsigned crc = 0xFFFFu;
  int i;
  printf("scale=%d\n", scale(seed, 7));
  printf("scale=%d\n", scale(-seed, 3));
  for (i = 0; i < 8; ++i)
    crc = crc_step(crc, (unsigned char)(seed * 31 + i * 17));
  printf("crc=%u\n", crc);
  printf("widen=%lld\n", widen(seed * 1000003, seed + 7));
  printf("blend=%.6f\n", blend((double)seed * 0.5, (float)seed * 0.25f));
  printf("mix6=%d\n", mix6(seed, seed + 1, seed + 2, seed + 3, seed + 4,
                           seed + 5));
  printf("mix8=%d\n", mix8(seed, seed + 1, seed + 2, seed + 3, seed + 4,
                           seed + 5, seed + 6, seed + 7));
  printf("mixfd=%.6f\n", mixfd(seed, (double)seed + 0.125, seed * 2,
                               (float)seed * 1.5f, seed * 3));
}
