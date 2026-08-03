// Consuming TU for the FR-59 globals invariant test: reads the extern
// global defined in the OTHER directory, so the naive per-directory
// partition would put the global and this reader in different crates --
// which FR-51 forbids widening for (globals are never exported) and the
// planner must condense instead.

extern int shared_g;

int getb(void) { return shared_g + 1; }
