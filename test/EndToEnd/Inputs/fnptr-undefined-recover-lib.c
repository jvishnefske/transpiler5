/* Companion TU for fnptr-undefined-recover.c: carries the FR-77 hazard --
 * a file-scope fn-ptr initializer naming a function NO TU on the compile
 * line defines (the initializer is its only reference) -- next to a
 * perfectly translatable function the main TU calls. Recovery must drop
 * g_rng and stub its readers while `offset` still ports. */
typedef int (*RNG)(unsigned char *dest, unsigned int size);
extern int default_csprng(unsigned char *dest, unsigned int size);

static RNG g_rng = &default_csprng;

void set_rng(RNG f) { g_rng = f; }
RNG get_rng(void) { return g_rng; }
int gen(unsigned char *buf, unsigned int n) {
  RNG f = g_rng;
  return f ? f(buf, n) : 0;
}

int offset(int x) { return x + 4; }
