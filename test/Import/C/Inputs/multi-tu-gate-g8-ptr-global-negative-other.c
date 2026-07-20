// Companion for multi-tu-gate-g8-ptr-global-negative.c: rebinds the SAME
// externally visible `g` to a DIFFERENT global (`B`, not `A`) and reads
// it — the genuinely divergent cross-TU pointer-global rebinding that
// must stay rejected even once a whole-program `globalPtrFacts` merge
// exists.
extern int *g;
int B[4];

void rebind(void) { g = &B[0]; }
int use_g(void) { return *g; }
int main(void) {
  rebind();
  return use_g();
}
