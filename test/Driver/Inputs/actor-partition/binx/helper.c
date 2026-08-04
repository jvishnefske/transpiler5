/* Actor cluster X: an extern global plus its arms. The main TU reads
   x_count directly, so FR-59's globals invariant condenses this TU into
   the binary crate -- the whole cluster sits in the bin unit set and is
   liftable under --partition (FR-62 F1b). */
int x_count = 0;

void x_bump(int by) { x_count += by; }
int x_value(void) { return x_count; }
