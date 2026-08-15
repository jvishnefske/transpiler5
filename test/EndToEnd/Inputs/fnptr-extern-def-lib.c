/* Companion TU for fnptr-extern-def.c: the definition of the function whose
 * address the main TU's file-scope initializer takes. Deterministic byte
 * pattern so the differential diff sees every element. */
int default_csprng(unsigned char *dest, unsigned int size) {
  unsigned int i;
  for (i = 0; i < size; ++i)
    dest[i] = (unsigned char)(i * 7u + 3u);
  return 1;
}
