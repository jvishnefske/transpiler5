/* Actor cluster X: an extern global plus its arms. The main TU reads
   x_count directly, so FR-59's globals invariant condenses this TU into
   the binary crate and the FR-62 F1b bin-local lift owns the cluster. */
int x_count = 0;

void x_bump(int by) { x_count += by; }
int x_value(void) { return x_count; }
