/* FR-202: the ONE driver body of the c-abi-exports-slice dlopen end-to-end
   test, compiled twice so the two legs cannot drift apart.

   In the NATIVE leg it is included by the library source itself, so every call
   binds to the C function directly. In the dlopen leg it is included by the
   host after the host has declared file-scope FUNCTION POINTERS under the very
   same names and filled them from dlsym, so the identical call syntax binds to
   the emitted cdylib's bare C symbols. A difference in the printed bytes can
   therefore only come from the CROSSING.

   Every value fed in derives from `seed`, which derives from argc, so nothing
   here can be constant-folded on either side.

   THE BUFFERS ARE EXACTLY THE PROVEN BOUND, never a byte more. That is the
   point of the whole increment: the wrapper builds the slice from a length it
   proved out of the callee's body, and a length larger than the bound would be
   reading storage this driver never allocated. It is also why `bump_read`
   matters -- it reads index 3 of a four-byte buffer, so a bound computed one
   too SMALL is an immediate Rust index panic in a function that cannot unwind,
   i.e. SIGABRT and a loudly empty diff rather than a quiet wrong number.

   `byte_pick` and `lead_scalar` are the argument-slot oracles. FR-181 measured
   that exporting `&[u8]` directly makes Rust read the length out of the
   caller's NEXT argument and shift everything after it; if that ever happened
   here, `byte_pick`'s `k` would be garbage and its product would differ from
   the native's on every seed. */

static void run_driver(int seed) {
  unsigned char h[3];
  unsigned char two[2];
  unsigned char four[4];
  int i;

  /* A synthetic MPEG frame header. `(h[1] >> 1) & 3` must not be zero (the
     C indexes `halfrate[..][that - 1]`) and `h[2] >> 4` must stay under 15,
     so both are constructed in range and the vector is UB-free on both legs. */
  h[0] = (unsigned char)(0xE0 | (seed & 0x0F));
  h[1] = (unsigned char)(((seed & 1) << 3) | ((((seed % 3) + 1) & 3) << 1));
  h[2] = (unsigned char)(((seed % 15) & 0x0F) << 4);
  printf("hdr_bitrate=%u\n", hdr_bitrate(h));

  for (i = 0; i < 2; ++i)
    two[i] = (unsigned char)(seed * 7 + i * 13 + 1);
  printf("byte_pick=%d\n", byte_pick(two, seed * 3 + 1));
  printf("lead_scalar=%d\n", lead_scalar(seed * 5, two));

  for (i = 0; i < 4; ++i)
    four[i] = (unsigned char)(seed * 11 + i * 29 + 2);
  printf("bump_read=%u\n", bump_read(four));
}
