// Companion TU for test/EndToEnd/link-orphan-extern-global-e2e.c (not
// discovered as a test). It DEFINES `vl_iface`, but the definition is
// dropped at import -- `unsupported: non-constant global initializer`,
// because the flexible-array tail holds pointers -- which is precisely why
// the other TU's obligation cannot resolve at link. `report` deliberately
// does NOT read `vl_iface`, so it survives and carries the observable value.
struct Sym { int tag; };
struct Iface { int n; const struct Sym *syms[]; };
extern const struct Iface vl_iface;
int report(int seed);

static const struct Sym s1 = { 1 };
static const struct Sym s2 = { 2 };
const struct Iface vl_iface = { 2, { &s1, &s2, 0 } };

int report(int seed) { return seed * 3 + 9; }
