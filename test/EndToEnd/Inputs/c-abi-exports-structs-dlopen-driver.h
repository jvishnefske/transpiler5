/* FR-182: the ONE driver body of the c-abi-exports-structs dlopen end-to-end
   test, compiled twice so the two legs cannot drift apart.

   In the NATIVE leg it is included by the library source itself, so every call
   binds to the C function directly. In the dlopen leg it is included by the
   host after the host has declared file-scope FUNCTION POINTERS under the very
   same names and filled them from dlsym, so the identical call syntax binds to
   the emitted cdylib's bare C symbols. A difference in the printed bytes can
   therefore only come from the CROSSING.

   Every value fed in derives from `seed`, which derives from argc, so no
   result can be constant-folded on either side. The floats are exact binary
   fractions and the integer arithmetic stays in range, so the output is
   deterministic and UB-free.

   `tflac_validate` is the load-bearing one: the struct is allocated HERE, by
   the C compiler, and the library writes three of its fields through the
   pointer. Reading all five back afterwards is what makes a field-offset
   disagreement visible rather than merely possible. */

static void run_driver(int seed) {
  vec2 a, b, s;
  struct wide w;
  struct big g, h;
  struct tflac t;
  struct span sp;
  int rc;

  a.x = (float)seed * 1.5f;
  a.y = (float)seed * 0.25f;
  b.x = (float)seed + 0.75f;
  b.y = (float)seed * 2.0f;
  s = vec_add(a, b);
  printf("vec_add=%.6f %.6f\n", (double)s.x, (double)s.y);
  printf("vec_dot=%.6f\n", (double)vec_dot(a, b));

  w.a = seed * 7;
  w.b = (double)seed * 0.125 + 1.0;
  w.c = seed - 3;
  printf("wide_mix=%.6f\n", wide_mix(w, seed * 11));

  g.a = seed;
  g.b = seed + 1;
  g.c = seed + 2;
  g.d = seed + 3;
  g.e = seed + 4;
  g.f = seed + 5;
  h = big_shift(g, seed * 100);
  printf("big_shift=%d %d %d %d %d %d\n", h.a, h.b, h.c, h.d, h.e, h.f);
  printf("big_sum=%d\n", big_sum(h));

  printf("mix_after=%d\n",
         mix_after(seed, a, seed + 1, (double)seed * 3.0, b, seed + 2));

  t.blocksize = (unsigned)(seed * 64);
  t.samplerate = (unsigned)(seed * 1000);
  t.channel_mode = 0;
  t.partition_order = 0;
  t.cur_blocksize = 0;
  rc = tflac_validate(&t, seed * 3);
  printf("tflac_validate=%d fields=%u %u %u %u %u\n", rc, t.blocksize,
         t.samplerate, (unsigned)t.channel_mode, (unsigned)t.partition_order,
         t.cur_blocksize);
  printf("tflac_peek=%u\n", tflac_peek(&t));

  sp.lo = a;
  sp.hi = b;
  printf("span_width=%.6f\n", (double)span_width(&sp));
}
