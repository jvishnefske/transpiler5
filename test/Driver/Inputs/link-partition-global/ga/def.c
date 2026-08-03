// Defining TU for the FR-59 globals invariant test: an external global
// plus its in-directory reader.

int shared_g = 5;

int geta(void) { return shared_g; }
