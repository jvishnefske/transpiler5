/* FR-208: the ONE driver body of the c-abi-exports-mixed-case dlopen
   end-to-end test, compiled three times so the legs cannot drift apart.

   In the NATIVE leg it is included by the library source itself, so every
   call below binds to the C function directly. In each dlopen leg (the
   idiomatic-rename crate and the `--preserve-c-names` crate) it is included
   by the host after the host has declared file-scope FUNCTION POINTERS under
   the very same names and filled them from dlsym, so the identical call
   syntax binds to the emitted cdylib's bare C symbols.

   Every name called here is spelled EXACTLY as the C source spells it --
   `SPX_add`, `SPX_crcStep`, `spxWiden` -- which is the whole point: before
   FR-208 the emitted shared object carried the FR-53 idiomatic rename
   (`spx_add`, `spx_crc_step`, `spx_widen`) and every one of these dlsyms
   returned NULL.

   Every value fed in derives from `seed`, which derives from argc, so no
   result can be constant-folded on either side. */

static void run_driver(int seed) {
  unsigned crc = 0xFFFFu;
  int i;
  spx_pair p;
  printf("add=%d\n", SPX_add(seed, seed * 7 + 1));
  printf("add=%d\n", SPX_add(-seed, 3));
  for (i = 0; i < 8; ++i)
    crc = SPX_crcStep(crc, (unsigned char)(seed * 31 + i * 17));
  printf("crc=%u\n", crc);
  printf("widen=%lld\n", spxWiden(seed * 1000003, seed + 7));
  printf("blend=%.6f\n", SPX_blend((double)seed * 0.5, (float)seed * 0.25f));
  printf("mix8=%d\n", SPX_mix8(seed, seed + 1, seed + 2, seed + 3, seed + 4,
                               seed + 5, seed + 6, seed + 7));
  printf("plain=%d\n", plain_add(seed * 11, seed));
  p.lo = seed + 2;
  p.hi = seed * 5;
  printf("pairsum=%d\n", SPX_pair_sum(&p));
  SPX_pair_scale(&p, seed + 3);
  printf("pair=%d %d\n", p.lo, p.hi);
  printf("pairsum=%d\n", SPX_pair_sum(&p));
}
